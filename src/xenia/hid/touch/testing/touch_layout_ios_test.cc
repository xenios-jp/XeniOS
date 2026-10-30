/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/touch/touch_layout_ios.h"

#include <algorithm>

#include "third_party/catch/include/catch.hpp"
#include "xenia/hid/input.h"
#include "xenia/hid/touch/touch_input_resolver.h"
#include "xenia/hid/touch/touch_layout_editor.h"

namespace xe::hid::touch::test {
namespace {

const IOSTouchControlDefinition* FindControlByAction(
    const IOSTouchLayoutModel& layout, IOSTouchAction action) {
  auto it = std::find_if(layout.controls.begin(), layout.controls.end(),
                         [action](const IOSTouchControlDefinition& control) {
                           return control.action == action;
                         });
  return it == layout.controls.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("Default iOS FPS touch layout is valid") {
  IOSTouchLayoutModel layout = CreateDefaultIOSFPSLayoutModel();

  REQUIRE(IsValidIOSTouchLayoutModel(layout));
  REQUIRE_FALSE(layout.layout_id.empty());
  REQUIRE(layout.controls.size() >= 6);
  REQUIRE(FindControlByAction(layout, IOSTouchAction::kMove) != nullptr);
  REQUIRE(FindControlByAction(layout, IOSTouchAction::kLook) != nullptr);
  REQUIRE(FindControlByAction(layout, IOSTouchAction::kButtonA) != nullptr);
  REQUIRE(FindControlByAction(layout, IOSTouchAction::kRightTrigger) !=
          nullptr);
}

TEST_CASE("Touch action mapping updates XInput output fields") {
  IOSTouchControlDefinition control;

  REQUIRE(ConfigureIOSTouchControlAction(IOSTouchAction::kButtonA, &control));
  REQUIRE(control.mapped_buttons == X_INPUT_GAMEPAD_A);
  REQUIRE(control.mapped_left_trigger == 0);
  REQUIRE(control.mapped_right_trigger == 0);

  REQUIRE(
      ConfigureIOSTouchControlAction(IOSTouchAction::kLeftTrigger, &control));
  REQUIRE(control.mapped_buttons == 0);
  REQUIRE(control.mapped_left_trigger == 0xFF);
  REQUIRE(control.mapped_right_trigger == 0);
  REQUIRE(control.drag_output == IOSTouchAnalogOutput::kLook);
  REQUIRE(control.analog_tuning.horizontal_scale == Approx(0.80f));

  REQUIRE(
      ConfigureIOSTouchControlAction(IOSTouchAction::kRightTrigger, &control));
  REQUIRE(control.mapped_buttons == 0);
  REQUIRE(control.mapped_left_trigger == 0);
  REQUIRE(control.mapped_right_trigger == 0xFF);
  REQUIRE(control.drag_output == IOSTouchAnalogOutput::kLook);
  REQUIRE(control.analog_tuning.horizontal_scale == Approx(0.92f));
}

TEST_CASE("Touch input resolver hit-tests rounded and circular controls") {
  IOSTouchControlDefinition control;
  control.shape = IOSTouchControlShape::kCircle;
  const IOSTouchRect square_frame{0.0f, 0.0f, 100.0f, 100.0f};

  REQUIRE(TouchControlContainsPoint(control, square_frame,
                                    IOSTouchPoint{50.0f, 50.0f}));
  REQUIRE_FALSE(TouchControlContainsPoint(control, square_frame,
                                          IOSTouchPoint{1.0f, 1.0f}));

  const IOSTouchRect capsule_frame{0.0f, 0.0f, 160.0f, 80.0f};
  REQUIRE(TouchControlContainsPoint(control, capsule_frame,
                                    IOSTouchPoint{80.0f, 40.0f}));
  REQUIRE_FALSE(TouchControlContainsPoint(control, capsule_frame,
                                          IOSTouchPoint{5.0f, 5.0f}));

  control.shape = IOSTouchControlShape::kRoundedRect;
  REQUIRE(TouchControlContainsPoint(control, square_frame,
                                    IOSTouchPoint{1.0f, 1.0f}));
  REQUIRE_FALSE(TouchControlContainsPoint(control, square_frame,
                                          IOSTouchPoint{101.0f, 50.0f}));
}

TEST_CASE("Touch input resolver resolves Move and D-pad combo zones") {
  IOSTouchControlDefinition control;
  control.type = IOSTouchControlType::kMoveStick;
  control.move_with_dpad_ring = true;
  const IOSTouchRect frame{0.0f, 0.0f, 100.0f, 100.0f};

  REQUIRE(
      ResolveTouchComboSubzone(control, frame, IOSTouchPoint{50.0f, 50.0f}) ==
      IOSTouchComboSubzone::kStick);
  REQUIRE(
      ResolveTouchComboSubzone(control, frame, IOSTouchPoint{50.0f, 5.0f}) ==
      IOSTouchComboSubzone::kDpadUp);
  REQUIRE(
      ResolveTouchComboSubzone(control, frame, IOSTouchPoint{50.0f, 95.0f}) ==
      IOSTouchComboSubzone::kDpadDown);
  REQUIRE(
      ResolveTouchComboSubzone(control, frame, IOSTouchPoint{5.0f, 50.0f}) ==
      IOSTouchComboSubzone::kDpadLeft);
  REQUIRE(
      ResolveTouchComboSubzone(control, frame, IOSTouchPoint{95.0f, 50.0f}) ==
      IOSTouchComboSubzone::kDpadRight);

  control.move_with_dpad_ring = false;
  REQUIRE(
      ResolveTouchComboSubzone(control, frame, IOSTouchPoint{95.0f, 50.0f}) ==
      IOSTouchComboSubzone::kStick);
}

TEST_CASE("Touch input resolver maps actions to XInput state") {
  IOSTouchResolvedState state;

  ApplyTouchActionMappingForAction(IOSTouchAction::kButtonA, &state);
  REQUIRE(state.buttons == X_INPUT_GAMEPAD_A);
  REQUIRE(state.left_trigger == 0);
  REQUIRE(state.right_trigger == 0);

  ApplyTouchActionMappingForAction(IOSTouchAction::kLeftTrigger, &state);
  REQUIRE(state.buttons == X_INPUT_GAMEPAD_A);
  REQUIRE(state.left_trigger == 0xFF);
  REQUIRE(state.right_trigger == 0);

  IOSTouchControlDefinition control;
  REQUIRE(
      ConfigureIOSTouchControlAction(IOSTouchAction::kRightBumper, &control));
  ApplyTouchActionMapping(control, &state);
  REQUIRE(state.buttons ==
          (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_RIGHT_SHOULDER));
}

TEST_CASE("Touch input resolver clamps swipe-look vectors") {
  IOSTouchPoint clamped = TouchSwipeLookVectorForDelta(
      IOSTouchPoint{12.0f, -3.0f}, 2.0f, 4.0f, 1.5f);
  REQUIRE(clamped.x == Approx(1.0f));
  REQUIRE(clamped.y == Approx(1.0f));

  IOSTouchPoint scaled =
      TouchSwipeLookVectorForDelta(IOSTouchPoint{2.0f, 1.0f}, 0.5f, 4.0f, 2.0f);
  REQUIRE(scaled.x == Approx(0.25f));
  REQUIRE(scaled.y == Approx(-0.25f));
}

TEST_CASE("Touch analog tuning acceleration boosts high velocity swipes") {
  IOSTouchAnalogTuning tuning;
  tuning.acceleration_scale = 1.0f;
  tuning.max_output = 1.0f;

  IOSTouchPoint normal = ApplyTouchAnalogTuningWithVelocity(
      IOSTouchPoint{0.20f, 0.0f}, tuning, 0.0f);
  IOSTouchPoint fast = ApplyTouchAnalogTuningWithVelocity(
      IOSTouchPoint{0.20f, 0.0f}, tuning, 80.0f);

  REQUIRE(normal.x == Approx(0.20f));
  REQUIRE(fast.x == Approx(0.40f));
  REQUIRE(fast.y == Approx(0.0f));
}

TEST_CASE("Touch input resolver resolves deferred and hold-drag behavior") {
  IOSTouchControlDefinition control;
  control.type = IOSTouchControlType::kActionButton;
  control.secondary_behavior.trigger = IOSTouchInteractionTrigger::kHold;
  control.secondary_behavior.action = IOSTouchAction::kButtonX;

  REQUIRE(TouchControlUsesDeferredPrimaryTap(control));
  control.hold_while_captured = true;
  REQUIRE_FALSE(TouchControlUsesDeferredPrimaryTap(control));

  IOSTouchInteractionBehavior behavior;
  behavior.trigger = IOSTouchInteractionTrigger::kHoldDrag;
  behavior.action = IOSTouchAction::kButtonY;
  behavior.enables_relative_look = true;
  behavior.relative_look_scale = 3.0f;
  behavior.hold_seconds = 0.10f;
  behavior.drag_threshold_points = 10.0f;

  IOSTouchInputCapture capture;
  capture.anchor_point = IOSTouchPoint{10.0f, 10.0f};
  capture.current_point = IOSTouchPoint{25.0f, 10.0f};
  capture.began_time = 1.0;

  IOSTouchInteractionBehaviorState state =
      ResolveTouchInteractionBehaviorState(behavior, capture, 1.20);
  REQUIRE(state.active);
  REQUIRE(state.enables_relative_look);
  REQUIRE(state.analog_output == IOSTouchAnalogOutput::kLook);
  REQUIRE(state.relative_look_scale == Approx(2.0f));
  REQUIRE(state.analog_tuning.horizontal_scale == Approx(2.0f));

  capture.current_point = IOSTouchPoint{12.0f, 10.0f};
  state = ResolveTouchInteractionBehaviorState(behavior, capture, 1.20);
  REQUIRE_FALSE(state.active);
}

TEST_CASE("Touch input resolver qualifies Move stick double-tap forward") {
  IOSTouchControlDefinition control;
  control.type = IOSTouchControlType::kMoveStick;
  control.activation_radius = 0.50f;
  control.deadzone = 0.0f;
  const IOSTouchRect frame{0.0f, 0.0f, 100.0f, 100.0f};

  IOSTouchInputCapture capture;
  capture.anchor_point = IOSTouchPoint{50.0f, 50.0f};
  capture.current_point = IOSTouchPoint{50.0f, 0.0f};
  capture.began_time = 1.0;

  REQUIRE(MoveStickCaptureQualifiesForDoubleTapForward(control, frame, capture,
                                                       1.20));
  REQUIRE_FALSE(MoveStickCaptureQualifiesForDoubleTapForward(control, frame,
                                                             capture, 1.50));

  capture.current_point = IOSTouchPoint{90.0f, 50.0f};
  REQUIRE_FALSE(MoveStickCaptureQualifiesForDoubleTapForward(control, frame,
                                                             capture, 1.20));
}

TEST_CASE("Touch input resolver compares states without packet churn") {
  IOSTouchResolvedState left;
  left.packet_number = 1;
  left.buttons = X_INPUT_GAMEPAD_A;
  left.thumb_lx = 12;
  left.gameplay_enabled = true;

  IOSTouchResolvedState right = left;
  right.packet_number = 99;
  REQUIRE(TouchStatesEqualIgnoringPacket(left, right));

  right.thumb_lx = 13;
  REQUIRE_FALSE(TouchStatesEqualIgnoringPacket(left, right));
}

TEST_CASE("Touch layout TOML round trip preserves editable model state") {
  IOSTouchLayoutModel layout = CreateDefaultIOSFPSLayoutModel();
  REQUIRE(!layout.controls.empty());
  IOSTouchControlDefinition& control = layout.controls.front();
  SetIOSTouchControlCustomLabel("  Move  ", &control);
  control.has_portrait_frame = true;
  control.portrait_normalized_frame = IOSTouchRect{0.2f, 0.3f, 0.4f, 0.5f};
  control.tint_style = IOSTouchTintStyle::kMint;
  control.analog_tuning.horizontal_scale = 1.50f;
  control.analog_tuning.vertical_scale = 0.75f;
  control.analog_tuning.acceleration_scale = 0.50f;
  control.analog_tuning.smoothing = 0.35f;
  control.held_move_scale = 0.50f;

  toml::table encoded = EncodeIOSTouchLayoutModel(layout);
  IOSTouchLayoutModel decoded;
  REQUIRE(ApplyIOSTouchLayoutModel(encoded, &decoded));

  REQUIRE(decoded.layout_id == layout.layout_id);
  REQUIRE(decoded.display_name == layout.display_name);
  REQUIRE(decoded.controls.size() == layout.controls.size());
  const IOSTouchControlDefinition& decoded_control = decoded.controls.front();
  REQUIRE(decoded_control.label == "Move");
  REQUIRE_FALSE(decoded_control.label_uses_default);
  REQUIRE(decoded_control.has_portrait_frame);
  REQUIRE(decoded_control.portrait_normalized_frame.x == Approx(0.2f));
  REQUIRE(decoded_control.portrait_normalized_frame.y == Approx(0.3f));
  REQUIRE(decoded_control.portrait_normalized_frame.width == Approx(0.4f));
  REQUIRE(decoded_control.portrait_normalized_frame.height == Approx(0.5f));
  REQUIRE(decoded_control.tint_style == IOSTouchTintStyle::kMint);
  REQUIRE(decoded_control.analog_tuning.horizontal_scale == Approx(1.50f));
  REQUIRE(decoded_control.analog_tuning.vertical_scale == Approx(0.75f));
  REQUIRE(decoded_control.analog_tuning.acceleration_scale == Approx(0.50f));
  REQUIRE(decoded_control.analog_tuning.smoothing == Approx(0.35f));
  REQUIRE(decoded_control.held_move_scale == Approx(0.50f));
}

TEST_CASE("Touch runtime model publishes resolved state snapshots") {
  IOSTouchRuntimeModel runtime_model;

  IOSTouchResolvedState state;
  state.packet_number = 7;
  state.buttons = X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B;
  state.left_trigger = 31;
  state.right_trigger = 255;
  state.thumb_lx = -1234;
  state.thumb_ly = 5678;
  state.gameplay_enabled = true;
  runtime_model.StoreResolvedState(state);

  IOSTouchResolvedState loaded = runtime_model.LoadResolvedState();
  REQUIRE(loaded.packet_number == 7);
  REQUIRE(loaded.buttons == (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B));
  REQUIRE(loaded.left_trigger == 31);
  REQUIRE(loaded.right_trigger == 255);
  REQUIRE(loaded.thumb_lx == -1234);
  REQUIRE(loaded.thumb_ly == 5678);
  REQUIRE(loaded.gameplay_enabled);

  runtime_model.ResetResolvedState();
  loaded = runtime_model.LoadResolvedState();
  REQUIRE(loaded.packet_number == 0);
  REQUIRE(loaded.buttons == 0);
  REQUIRE_FALSE(loaded.gameplay_enabled);
}

TEST_CASE("Touch layout editor adds and duplicates action buttons") {
  IOSTouchLayoutModel layout = CreateDefaultIOSFPSLayoutModel();
  const size_t initial_count = layout.controls.size();

  std::string added_identifier;
  REQUIRE(AddSuggestedActionButtonToIOSTouchLayout(&layout, true,
                                                   &added_identifier));
  REQUIRE(layout.controls.size() == initial_count + 1);
  REQUIRE_FALSE(added_identifier.empty());
  const IOSTouchControlDefinition& added = layout.controls.back();
  REQUIRE(added.identifier == added_identifier);
  REQUIRE(added.type == IOSTouchControlType::kActionButton);
  REQUIRE(added.action == IOSTouchAction::kButtonB);
  REQUIRE(added.has_portrait_frame);
  REQUIRE(added.portrait_normalized_frame.x ==
          Approx(added.normalized_frame.x));
  const IOSTouchAction added_action = added.action;
  const float added_portrait_x = added.portrait_normalized_frame.x;

  std::string duplicate_identifier;
  REQUIRE(DuplicateIOSTouchLayoutActionButton(
      &layout, layout.controls.size() - 1, true, &duplicate_identifier));
  REQUIRE(layout.controls.size() == initial_count + 2);
  const IOSTouchControlDefinition& duplicate = layout.controls.back();
  REQUIRE(duplicate.identifier == duplicate_identifier);
  REQUIRE(duplicate_identifier != added_identifier);
  REQUIRE(duplicate.action == added_action);
  REQUIRE(duplicate.portrait_normalized_frame.x ==
          Approx(std::min(added_portrait_x + 0.03f,
                          1.0f - duplicate.portrait_normalized_frame.width)));
}

TEST_CASE("Touch layout editor mirrors copies and deletes controls") {
  IOSTouchLayoutModel layout = CreateDefaultIOSFPSLayoutModel();
  REQUIRE(layout.controls.size() > 1);
  IOSTouchControlDefinition& control = layout.controls.front();
  control.normalized_frame = IOSTouchRect{0.10f, 0.20f, 0.30f, 0.40f};

  REQUIRE(MirrorIOSTouchLayoutControlHorizontally(&layout, 0, false));
  REQUIRE(layout.controls.front().normalized_frame.x == Approx(0.60f));

  REQUIRE(CopyIOSTouchLayoutFramesAcrossOrientations(&layout, true));
  REQUIRE(layout.controls.front().has_portrait_frame);
  REQUIRE(layout.controls.front().portrait_normalized_frame.x == Approx(0.60f));

  REQUIRE(CopyIOSTouchLayoutFramesAcrossOrientations(&layout, false));
  REQUIRE(layout.controls.front().normalized_frame.x == Approx(0.60f));

  const size_t before_delete_count = layout.controls.size();
  REQUIRE(DeleteIOSTouchLayoutControl(&layout, 0));
  REQUIRE(layout.controls.size() == before_delete_count - 1);

  IOSTouchLayoutModel single_control_layout;
  single_control_layout.controls.push_back(IOSTouchControlDefinition{});
  REQUIRE_FALSE(DeleteIOSTouchLayoutControl(&single_control_layout, 0));
  REQUIRE(single_control_layout.controls.size() == 1);
}

}  // namespace xe::hid::touch::test
