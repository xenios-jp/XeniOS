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
#include <cmath>
#include <optional>
#include <unordered_set>
#include <utility>

namespace xe {
namespace hid {
namespace touch {

namespace {

constexpr float kMinControlSize = 0.05f;
constexpr float kMaxControlSize = 0.98f;
constexpr float kLegacyTouchBehaviorHoldSeconds = 0.18f;
constexpr int64_t kCurrentTouchLayoutSchemaVersion = 5;
constexpr size_t kMaxTouchControlIdentifierLength = 64;

bool UsesLegacyTouchBehaviorHoldDefault(float hold_seconds) {
  return std::abs(hold_seconds - kLegacyTouchBehaviorHoldSeconds) < 0.001f;
}

const char* IOSTouchControlTypeConfigName(IOSTouchControlType type) {
  switch (type) {
    case IOSTouchControlType::kMoveStick:
      return "move_stick";
    case IOSTouchControlType::kLookSwipeZone:
      return "look_zone";
    case IOSTouchControlType::kActionButton:
      return "action_button";
    case IOSTouchControlType::kPauseButton:
      return "pause_button";
  }
  return "action_button";
}

bool ParseIOSTouchControlTypeConfigName(const std::string& value,
                                        IOSTouchControlType* type_out) {
  if (!type_out) {
    return false;
  }
  if (value == "move_stick") {
    *type_out = IOSTouchControlType::kMoveStick;
    return true;
  }
  if (value == "look_zone") {
    *type_out = IOSTouchControlType::kLookSwipeZone;
    return true;
  }
  if (value == "pause_button") {
    *type_out = IOSTouchControlType::kPauseButton;
    return true;
  }
  if (value == "action_button") {
    *type_out = IOSTouchControlType::kActionButton;
    return true;
  }
  return false;
}

const char* IOSTouchControlShapeConfigName(IOSTouchControlShape shape) {
  switch (shape) {
    case IOSTouchControlShape::kCircle:
      return "circle";
    case IOSTouchControlShape::kRoundedRect:
      return "rounded_rect";
  }
  return "circle";
}

bool ParseIOSTouchControlShapeConfigName(const std::string& value,
                                         IOSTouchControlShape* shape_out) {
  if (!shape_out) {
    return false;
  }
  if (value == "circle") {
    *shape_out = IOSTouchControlShape::kCircle;
    return true;
  }
  if (value == "rounded_rect") {
    *shape_out = IOSTouchControlShape::kRoundedRect;
    return true;
  }
  return false;
}

const char* IOSTouchActionConfigName(IOSTouchAction action) {
  switch (action) {
    case IOSTouchAction::kNone:
      return "none";
    case IOSTouchAction::kMove:
      return "move";
    case IOSTouchAction::kLook:
      return "look";
    case IOSTouchAction::kPauseMenu:
      return "pause";
    case IOSTouchAction::kButtonA:
      return "a";
    case IOSTouchAction::kButtonB:
      return "b";
    case IOSTouchAction::kButtonX:
      return "x";
    case IOSTouchAction::kButtonY:
      return "y";
    case IOSTouchAction::kLeftBumper:
      return "lb";
    case IOSTouchAction::kRightBumper:
      return "rb";
    case IOSTouchAction::kLeftTrigger:
      return "lt";
    case IOSTouchAction::kRightTrigger:
      return "rt";
    case IOSTouchAction::kBack:
      return "back";
    case IOSTouchAction::kStart:
      return "start";
    case IOSTouchAction::kLeftThumb:
      return "ls";
    case IOSTouchAction::kRightThumb:
      return "rs";
    case IOSTouchAction::kDpadUp:
      return "dpad_up";
    case IOSTouchAction::kDpadDown:
      return "dpad_down";
    case IOSTouchAction::kDpadLeft:
      return "dpad_left";
    case IOSTouchAction::kDpadRight:
      return "dpad_right";
  }
  return "none";
}

bool ParseIOSTouchActionConfigName(const std::string& value,
                                   IOSTouchAction* action_out) {
  if (!action_out) {
    return false;
  }
  if (value == "move") {
    *action_out = IOSTouchAction::kMove;
    return true;
  }
  if (value == "look") {
    *action_out = IOSTouchAction::kLook;
    return true;
  }
  if (value == "pause") {
    *action_out = IOSTouchAction::kPauseMenu;
    return true;
  }
  if (value == "jump") {
    *action_out = IOSTouchAction::kButtonA;
    return true;
  }
  if (value == "reload_interact") {
    *action_out = IOSTouchAction::kButtonX;
    return true;
  }
  if (value == "aim") {
    *action_out = IOSTouchAction::kLeftTrigger;
    return true;
  }
  if (value == "fire") {
    *action_out = IOSTouchAction::kRightTrigger;
    return true;
  }
  for (IOSTouchAction action : kIOSTouchEditableActions) {
    if (value == IOSTouchActionConfigName(action)) {
      *action_out = action;
      return true;
    }
  }
  return false;
}

const char* IOSTouchInteractionTriggerConfigName(
    IOSTouchInteractionTrigger trigger) {
  switch (trigger) {
    case IOSTouchInteractionTrigger::kNone:
      return "none";
    case IOSTouchInteractionTrigger::kHold:
      return "hold";
    case IOSTouchInteractionTrigger::kHoldDrag:
      return "hold_drag";
    case IOSTouchInteractionTrigger::kDoubleTap:
      return "double_tap";
    case IOSTouchInteractionTrigger::kDoubleTapForward:
      return "double_tap_forward";
  }
  return "none";
}

bool ParseIOSTouchInteractionTriggerConfigName(
    const std::string& value, IOSTouchInteractionTrigger* trigger_out) {
  if (!trigger_out) {
    return false;
  }
  if (value == "none") {
    *trigger_out = IOSTouchInteractionTrigger::kNone;
    return true;
  }
  if (value == "hold") {
    *trigger_out = IOSTouchInteractionTrigger::kHold;
    return true;
  }
  if (value == "hold_drag") {
    *trigger_out = IOSTouchInteractionTrigger::kHoldDrag;
    return true;
  }
  if (value == "double_tap") {
    *trigger_out = IOSTouchInteractionTrigger::kDoubleTap;
    return true;
  }
  if (value == "double_tap_forward") {
    *trigger_out = IOSTouchInteractionTrigger::kDoubleTapForward;
    return true;
  }
  return false;
}

const char* IOSTouchAnalogOutputConfigName(IOSTouchAnalogOutput output) {
  switch (output) {
    case IOSTouchAnalogOutput::kNone:
      return "none";
    case IOSTouchAnalogOutput::kLook:
      return "look";
    case IOSTouchAnalogOutput::kMove:
      return "move";
  }
  return "none";
}

bool ParseIOSTouchAnalogOutputConfigName(const std::string& value,
                                         IOSTouchAnalogOutput* output_out) {
  if (!output_out) {
    return false;
  }
  if (value == "none" || value == "off") {
    *output_out = IOSTouchAnalogOutput::kNone;
    return true;
  }
  if (value == "look") {
    *output_out = IOSTouchAnalogOutput::kLook;
    return true;
  }
  if (value == "move") {
    *output_out = IOSTouchAnalogOutput::kMove;
    return true;
  }
  return false;
}

const char* IOSTouchTintStyleConfigName(IOSTouchTintStyle tint_style) {
  switch (tint_style) {
    case IOSTouchTintStyle::kAuto:
      return "auto";
    case IOSTouchTintStyle::kAmber:
      return "amber";
    case IOSTouchTintStyle::kSky:
      return "sky";
    case IOSTouchTintStyle::kMint:
      return "mint";
    case IOSTouchTintStyle::kRose:
      return "rose";
    case IOSTouchTintStyle::kLime:
      return "lime";
    case IOSTouchTintStyle::kCoral:
      return "coral";
    case IOSTouchTintStyle::kSlate:
      return "slate";
  }
  return "auto";
}

bool ParseIOSTouchTintStyleConfigName(const std::string& value,
                                      IOSTouchTintStyle* tint_style_out) {
  if (!tint_style_out) {
    return false;
  }
  for (IOSTouchTintStyle tint_style : kIOSTouchEditableTintStyles) {
    if (value == IOSTouchTintStyleConfigName(tint_style)) {
      *tint_style_out = tint_style;
      return true;
    }
  }
  return false;
}

void ApplyDerivedTouchControlDefaults(IOSTouchControlDefinition* control) {
  if (!control) {
    return;
  }

  if (control->type == IOSTouchControlType::kMoveStick &&
      control->secondary_behavior.trigger ==
          IOSTouchInteractionTrigger::kDoubleTapForward &&
      control->secondary_behavior.action == IOSTouchAction::kNone) {
    control->secondary_behavior.action = IOSTouchAction::kLeftThumb;
  }
}

bool ValidateIOSTouchControlDefinitions(
    const std::vector<IOSTouchControlDefinition>& controls) {
  if (controls.empty() || controls.size() > kMaxIOSTouchControls) {
    return false;
  }

  std::unordered_set<std::string> identifiers;
  size_t move_stick_count = 0;
  size_t look_stick_count = 0;
  size_t look_zone_count = 0;
  size_t pause_button_count = 0;
  for (const auto& control : controls) {
    if (control.identifier.empty() ||
        control.identifier.size() > kMaxTouchControlIdentifierLength ||
        !identifiers.insert(control.identifier).second) {
      return false;
    }

    switch (control.type) {
      case IOSTouchControlType::kMoveStick:
        if (control.action == IOSTouchAction::kLook) {
          ++look_stick_count;
        } else {
          ++move_stick_count;
        }
        break;
      case IOSTouchControlType::kLookSwipeZone:
        ++look_zone_count;
        break;
      case IOSTouchControlType::kPauseButton:
        ++pause_button_count;
        break;
      case IOSTouchControlType::kActionButton:
        break;
    }
  }

  return move_stick_count <= 1 && look_stick_count <= 1 &&
         look_zone_count <= 1 && pause_button_count <= 1;
}

float ClampNormalizedScalar(float value) {
  return std::clamp(value, 0.0f, 1.0f);
}

float ClampPositiveScalar(float value, float min_value, float max_value) {
  return std::clamp(value, min_value, max_value);
}

float ClampAnalogScale(float value) { return std::clamp(value, 0.1f, 4.0f); }

float ClampAnalogDeadzone(float value) {
  return std::clamp(value, 0.0f, 0.95f);
}

float ClampAnalogActivationRadius(float value) {
  return std::clamp(value, 0.05f, 1.0f);
}

float ClampAnalogMaxOutput(float value) {
  return std::clamp(value, 0.1f, 1.0f);
}

float ClampAnalogAcceleration(float value) {
  return std::clamp(value, 0.0f, 2.0f);
}

float ClampAnalogSmoothing(float value) {
  return std::clamp(value, 0.0f, 0.95f);
}

float MaxNormalizedControlSize(IOSTouchControlType control_type) {
  return control_type == IOSTouchControlType::kLookSwipeZone ? 1.0f
                                                             : kMaxControlSize;
}

IOSTouchRect ClampNormalizedRect(const IOSTouchRect& rect,
                                 IOSTouchControlType control_type) {
  IOSTouchRect result = rect;
  const float max_control_size = MaxNormalizedControlSize(control_type);
  result.width =
      ClampPositiveScalar(result.width, kMinControlSize, max_control_size);
  result.height =
      ClampPositiveScalar(result.height, kMinControlSize, max_control_size);
  result.x = std::clamp(result.x, 0.0f, 1.0f - result.width);
  result.y = std::clamp(result.y, 0.0f, 1.0f - result.height);
  return result;
}

void EncodeControlFrame(const IOSTouchRect& rect, toml::table* table) {
  if (!table) {
    return;
  }
  table->insert_or_assign("x", rect.x);
  table->insert_or_assign("y", rect.y);
  table->insert_or_assign("width", rect.width);
  table->insert_or_assign("height", rect.height);
}

void ApplyControlFrame(const toml::table& table, IOSTouchRect* rect,
                       IOSTouchControlType control_type) {
  if (!rect) {
    return;
  }

  IOSTouchRect next_rect = *rect;
  if (auto value = table["x"].value<double>()) {
    next_rect.x = static_cast<float>(*value);
  }
  if (auto value = table["y"].value<double>()) {
    next_rect.y = static_cast<float>(*value);
  }
  if (auto value = table["width"].value<double>()) {
    next_rect.width = static_cast<float>(*value);
  }
  if (auto value = table["height"].value<double>()) {
    next_rect.height = static_cast<float>(*value);
  }
  *rect = ClampNormalizedRect(next_rect, control_type);
}

void EncodeAnalogTuning(const IOSTouchAnalogTuning& tuning,
                        toml::table* table) {
  if (!table) {
    return;
  }
  table->insert_or_assign("deadzone", tuning.deadzone);
  table->insert_or_assign("activation_radius", tuning.activation_radius);
  table->insert_or_assign("horizontal_scale", tuning.horizontal_scale);
  table->insert_or_assign("vertical_scale", tuning.vertical_scale);
  table->insert_or_assign("diagonal_scale", tuning.diagonal_scale);
  table->insert_or_assign("response_curve", tuning.response_curve);
  table->insert_or_assign("acceleration_scale", tuning.acceleration_scale);
  table->insert_or_assign("smoothing", tuning.smoothing);
  table->insert_or_assign("max_output", tuning.max_output);
  table->insert_or_assign("invert_x", tuning.invert_x);
  table->insert_or_assign("invert_y", tuning.invert_y);
}

void ApplyAnalogTuning(const toml::table& table, IOSTouchAnalogTuning* tuning) {
  if (!tuning) {
    return;
  }
  if (auto value = table["deadzone"].value<double>()) {
    tuning->deadzone = ClampAnalogDeadzone(static_cast<float>(*value));
  }
  if (auto value = table["activation_radius"].value<double>()) {
    tuning->activation_radius =
        ClampAnalogActivationRadius(static_cast<float>(*value));
  }
  if (auto value = table["horizontal_scale"].value<double>()) {
    tuning->horizontal_scale = ClampAnalogScale(static_cast<float>(*value));
  }
  if (auto value = table["vertical_scale"].value<double>()) {
    tuning->vertical_scale = ClampAnalogScale(static_cast<float>(*value));
  }
  if (auto value = table["diagonal_scale"].value<double>()) {
    tuning->diagonal_scale = ClampAnalogScale(static_cast<float>(*value));
  }
  if (auto value = table["response_curve"].value<double>()) {
    tuning->response_curve = ClampAnalogScale(static_cast<float>(*value));
  }
  if (auto value = table["acceleration_scale"].value<double>()) {
    tuning->acceleration_scale =
        ClampAnalogAcceleration(static_cast<float>(*value));
  }
  if (auto value = table["smoothing"].value<double>()) {
    tuning->smoothing = ClampAnalogSmoothing(static_cast<float>(*value));
  }
  if (auto value = table["max_output"].value<double>()) {
    tuning->max_output = ClampAnalogMaxOutput(static_cast<float>(*value));
  }
  if (auto value = table["invert_x"].value<bool>()) {
    tuning->invert_x = *value;
  }
  if (auto value = table["invert_y"].value<bool>()) {
    tuning->invert_y = *value;
  }
}

void SyncLegacyLookScaleToAnalogTuning(float relative_look_scale,
                                       IOSTouchAnalogTuning* tuning) {
  if (!tuning) {
    return;
  }
  const float clamped_scale =
      ClampPositiveScalar(relative_look_scale, 0.25f, 4.0f);
  tuning->horizontal_scale = clamped_scale;
  tuning->vertical_scale = clamped_scale;
}

void FinalizeAnalogBehaviorFromLegacyFields(
    bool analog_output_overridden, bool analog_tuning_overridden,
    IOSTouchInteractionBehavior* behavior) {
  if (!behavior) {
    return;
  }
  if (!analog_output_overridden && behavior->enables_relative_look) {
    behavior->analog_output = IOSTouchAnalogOutput::kLook;
  }
  if (!analog_tuning_overridden &&
      behavior->analog_output == IOSTouchAnalogOutput::kLook) {
    SyncLegacyLookScaleToAnalogTuning(behavior->relative_look_scale,
                                      &behavior->analog_tuning);
  }
  behavior->enables_relative_look =
      behavior->analog_output == IOSTouchAnalogOutput::kLook;
  if (behavior->enables_relative_look) {
    behavior->relative_look_scale = behavior->analog_tuning.horizontal_scale;
  }
}

void FinalizeAnalogControlFromLegacyFields(bool drag_output_overridden,
                                           bool analog_tuning_overridden,
                                           IOSTouchControlDefinition* control) {
  if (!control) {
    return;
  }
  if (!drag_output_overridden) {
    if (control->enables_relative_look) {
      control->drag_output = IOSTouchAnalogOutput::kLook;
    } else if (control->type == IOSTouchControlType::kLookSwipeZone) {
      control->drag_output = control->action == IOSTouchAction::kMove
                                 ? IOSTouchAnalogOutput::kMove
                                 : IOSTouchAnalogOutput::kLook;
    }
  }
  if (!analog_tuning_overridden) {
    if (control->drag_output == IOSTouchAnalogOutput::kLook ||
        control->type == IOSTouchControlType::kLookSwipeZone) {
      SyncLegacyLookScaleToAnalogTuning(control->relative_look_scale,
                                        &control->analog_tuning);
    }
    control->analog_tuning.deadzone = control->deadzone;
    if (control->activation_radius > 0.0f) {
      control->analog_tuning.activation_radius = control->activation_radius;
    }
  } else {
    control->deadzone = control->analog_tuning.deadzone;
    control->activation_radius = control->analog_tuning.activation_radius;
  }
  control->enables_relative_look =
      control->drag_output == IOSTouchAnalogOutput::kLook;
  if (control->enables_relative_look ||
      control->type == IOSTouchControlType::kLookSwipeZone) {
    control->relative_look_scale = control->analog_tuning.horizontal_scale;
  }
}

void EncodeInteractionBehavior(const IOSTouchInteractionBehavior& behavior,
                               toml::table* table) {
  if (!table) {
    return;
  }
  table->insert_or_assign(
      "trigger",
      std::string(IOSTouchInteractionTriggerConfigName(behavior.trigger)));
  table->insert_or_assign(
      "action", std::string(IOSTouchActionConfigName(behavior.action)));
  table->insert_or_assign(
      "analog_output",
      std::string(IOSTouchAnalogOutputConfigName(behavior.analog_output)));
  toml::table tuning_table;
  EncodeAnalogTuning(behavior.analog_tuning, &tuning_table);
  table->insert_or_assign("analog_tuning", std::move(tuning_table));
  table->insert_or_assign("enables_relative_look",
                          behavior.enables_relative_look);
  table->insert_or_assign("relative_look_scale", behavior.relative_look_scale);
  table->insert_or_assign("hold_seconds", behavior.hold_seconds);
  table->insert_or_assign("drag_threshold_points",
                          behavior.drag_threshold_points);
}

void ApplyInteractionBehavior(const toml::table& table,
                              IOSTouchInteractionBehavior* behavior) {
  if (!behavior) {
    return;
  }

  bool hold_seconds_overridden = false;
  bool analog_output_overridden = false;
  bool analog_tuning_overridden = false;
  if (auto value = table["trigger"].value<std::string>()) {
    IOSTouchInteractionTrigger trigger = behavior->trigger;
    if (ParseIOSTouchInteractionTriggerConfigName(*value, &trigger)) {
      behavior->trigger = trigger;
    }
  }
  if (auto value = table["action"].value<std::string>()) {
    IOSTouchAction action = behavior->action;
    if (ParseIOSTouchActionConfigName(*value, &action)) {
      behavior->action = action;
    }
  }
  if (auto value = table["analog_output"].value<std::string>()) {
    IOSTouchAnalogOutput output = behavior->analog_output;
    if (ParseIOSTouchAnalogOutputConfigName(*value, &output)) {
      behavior->analog_output = output;
      analog_output_overridden = true;
    }
  }
  if (const toml::table* tuning_table = table["analog_tuning"].as_table()) {
    ApplyAnalogTuning(*tuning_table, &behavior->analog_tuning);
    analog_tuning_overridden = true;
  }
  if (auto value = table["enables_relative_look"].value<bool>()) {
    behavior->enables_relative_look = *value;
  }
  if (auto value = table["relative_look_scale"].value<double>()) {
    behavior->relative_look_scale =
        ClampPositiveScalar(static_cast<float>(*value), 0.1f, 2.0f);
  }
  if (auto value = table["hold_seconds"].value<double>()) {
    behavior->hold_seconds =
        ClampPositiveScalar(static_cast<float>(*value), 0.05f, 1.0f);
    hold_seconds_overridden = true;
  }
  if (auto value = table["drag_threshold_points"].value<double>()) {
    behavior->drag_threshold_points =
        ClampPositiveScalar(static_cast<float>(*value), 2.0f, 96.0f);
  }
  if (!hold_seconds_overridden ||
      UsesLegacyTouchBehaviorHoldDefault(behavior->hold_seconds)) {
    // Older layouts stored trigger-specific defaults directly, so preserve the
    // effective duration when migrating from those legacy values.
    behavior->hold_seconds =
        DefaultIOSTouchHoldSecondsForInteractionTrigger(behavior->trigger);
  }
  if (behavior->trigger == IOSTouchInteractionTrigger::kNone) {
    behavior->enables_relative_look = false;
    behavior->analog_output = IOSTouchAnalogOutput::kNone;
  } else if (behavior->trigger == IOSTouchInteractionTrigger::kHoldDrag &&
             !analog_output_overridden &&
             !table["enables_relative_look"].value<bool>().has_value()) {
    behavior->enables_relative_look = true;
    behavior->analog_output = IOSTouchAnalogOutput::kLook;
  }
  FinalizeAnalogBehaviorFromLegacyFields(analog_output_overridden,
                                         analog_tuning_overridden, behavior);
}

void ApplyStoredControlLabelState(const toml::table& control_table,
                                  IOSTouchControlDefinition* control) {
  if (!control) {
    return;
  }

  std::optional<std::string> stored_label;
  if (auto value = control_table["label"].value<std::string>()) {
    stored_label = TrimIOSTouchLabel(*value);
  }
  std::optional<bool> label_uses_default =
      control_table["label_uses_default"].value<bool>();
  std::optional<bool> label_hidden =
      control_table["label_hidden"].value<bool>();

  if (label_uses_default.has_value()) {
    if (*label_uses_default) {
      ResetIOSTouchControlLabel(control);
    } else if (stored_label.has_value()) {
      SetIOSTouchControlCustomLabel(*stored_label, control);
    } else if (!IOSTouchControlHasCustomLabel(*control)) {
      ResetIOSTouchControlLabel(control);
    }
  } else if (stored_label.has_value()) {
    if (stored_label->empty()) {
      ResetIOSTouchControlLabel(control);
      if (!label_hidden.has_value()) {
        control->label_hidden = true;
      }
    } else if (*stored_label == DefaultIOSTouchControlLabel(*control)) {
      ResetIOSTouchControlLabel(control);
    } else {
      SetIOSTouchControlCustomLabel(*stored_label, control);
    }
  }

  if (label_hidden.has_value()) {
    control->label_hidden = *label_hidden;
  }
}

IOSTouchControlDefinition DecodeIOSTouchControlDefinition(
    const toml::table& control_table, size_t control_index) {
  IOSTouchControlType control_type = IOSTouchControlType::kActionButton;
  if (auto value = control_table["type"].value<std::string>()) {
    ParseIOSTouchControlTypeConfigName(*value, &control_type);
  }

  IOSTouchControlDefinition control =
      MakeDefaultIOSTouchControlDefinitionImpl(control_type);
  bool drag_output_overridden = false;
  bool analog_tuning_overridden = false;
  if (auto value = control_table["id"].value<std::string>()) {
    control.identifier = *value;
  } else {
    control.identifier =
        std::string(IOSTouchControlTypeConfigName(control_type)) + "_" +
        std::to_string(control_index + 1);
  }
  if (auto value = control_table["shape"].value<std::string>()) {
    IOSTouchControlShape shape = control.shape;
    if (ParseIOSTouchControlShapeConfigName(*value, &shape)) {
      control.shape = shape;
    }
  }
  if (auto value = control_table["action"].value<std::string>()) {
    IOSTouchAction action = control.action;
    if (ParseIOSTouchActionConfigName(*value, &action) &&
        IsSupportedIOSTouchPrimaryAction(control.type, action)) {
      ConfigureIOSTouchControlAction(action, &control);
    }
  }
  ApplyStoredControlLabelState(control_table, &control);
  ApplyControlFrame(control_table, &control.normalized_frame, control.type);
  if (auto value = control_table["has_portrait_frame"].value<bool>()) {
    control.has_portrait_frame = *value;
  }
  if (control.has_portrait_frame) {
    if (const toml::table* portrait_frame_table =
            control_table["portrait_frame"].as_table()) {
      // Seed from the landscape frame so any missing axes inherit the
      // baseline rather than resetting to zero — matches the runtime
      // lazy-seed behaviour in MutableActiveControlFrameForOrientation.
      control.portrait_normalized_frame = control.normalized_frame;
      ApplyControlFrame(*portrait_frame_table,
                        &control.portrait_normalized_frame, control.type);
    } else {
      // `has_portrait_frame == true` without a `portrait_frame` block — the
      // override is meaningless, so treat it as unset and fall back to the
      // landscape frame at read time.
      control.has_portrait_frame = false;
    }
  }
  if (auto value = control_table["deadzone"].value<double>()) {
    control.deadzone = ClampNormalizedScalar(static_cast<float>(*value));
  }
  if (auto value = control_table["activation_radius"].value<double>()) {
    control.activation_radius =
        ClampPositiveScalar(static_cast<float>(*value), 0.0f, 1.0f);
  }
  if (auto value = control_table["visual_opacity"].value<double>()) {
    control.visual_opacity = ClampNormalizedScalar(static_cast<float>(*value));
  }
  if (auto value = control_table["tint_style"].value<std::string>()) {
    IOSTouchTintStyle tint_style = control.tint_style;
    if (ParseIOSTouchTintStyleConfigName(*value, &tint_style)) {
      control.tint_style = tint_style;
    }
  }
  if (auto value = control_table["hold_while_captured"].value<bool>()) {
    control.hold_while_captured = *value;
  }
  if (auto value = control_table["enables_relative_look"].value<bool>()) {
    control.enables_relative_look = *value;
  }
  if (auto value = control_table["drag_output"].value<std::string>()) {
    IOSTouchAnalogOutput output = control.drag_output;
    if (ParseIOSTouchAnalogOutputConfigName(*value, &output)) {
      control.drag_output = output;
      drag_output_overridden = true;
    }
  }
  if (const toml::table* tuning_table =
          control_table["analog_tuning"].as_table()) {
    ApplyAnalogTuning(*tuning_table, &control.analog_tuning);
    analog_tuning_overridden = true;
  }
  if (auto value = control_table["relative_look_scale"].value<double>()) {
    control.relative_look_scale =
        ClampPositiveScalar(static_cast<float>(*value), 0.25f, 4.0f);
  }
  if (auto value = control_table["held_look_scale"].value<double>()) {
    control.held_look_scale =
        ClampPositiveScalar(static_cast<float>(*value), 0.25f, 4.0f);
  }
  if (auto value = control_table["held_move_scale"].value<double>()) {
    control.held_move_scale =
        ClampPositiveScalar(static_cast<float>(*value), 0.25f, 4.0f);
  }
  if (auto value = control_table["move_with_dpad_ring"].value<bool>()) {
    control.move_with_dpad_ring = *value;
  }
  if (const toml::table* behavior_table =
          control_table["secondary_behavior"].as_table()) {
    ApplyInteractionBehavior(*behavior_table, &control.secondary_behavior);
  }
  if (auto value = control_table["capture_priority"].value<int64_t>()) {
    control.capture_priority =
        static_cast<uint8_t>(std::clamp<int64_t>(*value, 0, 255));
  }
  FinalizeAnalogControlFromLegacyFields(drag_output_overridden,
                                        analog_tuning_overridden, &control);
  ApplyDerivedTouchControlDefaults(&control);
  return control;
}
}  // namespace

toml::table EncodeIOSTouchLayoutModel(const IOSTouchLayoutModel& layout) {
  toml::table table;
  table.insert_or_assign("schema_version", kCurrentTouchLayoutSchemaVersion);
  table.insert_or_assign("layout_id", layout.layout_id);
  table.insert_or_assign("display_name", layout.display_name);
  table.insert_or_assign("author", layout.author);
  table.insert_or_assign("base_template", layout.base_template);

  toml::array controls_array;
  for (const auto& control : layout.controls) {
    toml::table control_table;
    control_table.insert_or_assign("id", control.identifier);
    control_table.insert_or_assign(
        "type", std::string(IOSTouchControlTypeConfigName(control.type)));
    control_table.insert_or_assign(
        "shape", std::string(IOSTouchControlShapeConfigName(control.shape)));
    control_table.insert_or_assign("label",
                                   IOSTouchConfiguredControlLabel(control));
    control_table.insert_or_assign("label_uses_default",
                                   control.label_uses_default);
    control_table.insert_or_assign("label_hidden", control.label_hidden);
    EncodeControlFrame(control.normalized_frame, &control_table);
    control_table.insert_or_assign("has_portrait_frame",
                                   control.has_portrait_frame);
    if (control.has_portrait_frame) {
      toml::table portrait_frame_table;
      EncodeControlFrame(control.portrait_normalized_frame,
                         &portrait_frame_table);
      control_table.insert_or_assign("portrait_frame",
                                     std::move(portrait_frame_table));
    }
    control_table.insert_or_assign(
        "action", std::string(IOSTouchActionConfigName(control.action)));
    control_table.insert_or_assign("deadzone", control.deadzone);
    control_table.insert_or_assign("activation_radius",
                                   control.activation_radius);
    control_table.insert_or_assign(
        "tint_style",
        std::string(IOSTouchTintStyleConfigName(control.tint_style)));
    control_table.insert_or_assign("visual_opacity", control.visual_opacity);
    control_table.insert_or_assign("hold_while_captured",
                                   control.hold_while_captured);
    control_table.insert_or_assign("enables_relative_look",
                                   control.enables_relative_look);
    control_table.insert_or_assign(
        "drag_output",
        std::string(IOSTouchAnalogOutputConfigName(control.drag_output)));
    toml::table analog_tuning_table;
    EncodeAnalogTuning(control.analog_tuning, &analog_tuning_table);
    control_table.insert_or_assign("analog_tuning",
                                   std::move(analog_tuning_table));
    control_table.insert_or_assign("relative_look_scale",
                                   control.relative_look_scale);
    control_table.insert_or_assign("held_look_scale", control.held_look_scale);
    control_table.insert_or_assign("held_move_scale", control.held_move_scale);
    control_table.insert_or_assign("move_with_dpad_ring",
                                   control.move_with_dpad_ring);
    toml::table secondary_behavior_table;
    EncodeInteractionBehavior(control.secondary_behavior,
                              &secondary_behavior_table);
    control_table.insert_or_assign("secondary_behavior",
                                   std::move(secondary_behavior_table));
    control_table.insert_or_assign(
        "capture_priority", static_cast<int64_t>(control.capture_priority));
    controls_array.push_back(std::move(control_table));
  }

  table.insert_or_assign("controls", std::move(controls_array));
  return table;
}

bool ApplyIOSTouchLayoutModel(const toml::table& table,
                              IOSTouchLayoutModel* layout) {
  if (!layout) {
    return false;
  }

  if (auto schema_version = table["schema_version"].value<int64_t>()) {
    if (*schema_version <= 0 ||
        *schema_version > kCurrentTouchLayoutSchemaVersion) {
      return false;
    }
  }

  if (auto value = table["layout_id"].value<std::string>()) {
    layout->layout_id = *value;
  }
  if (auto value = table["display_name"].value<std::string>()) {
    layout->display_name = *value;
  }
  if (auto value = table["author"].value<std::string>()) {
    layout->author = *value;
  }
  if (auto value = table["base_template"].value<std::string>()) {
    layout->base_template = *value;
  }

  const toml::array* controls_array = table["controls"].as_array();
  if (controls_array) {
    if (controls_array->size() > kMaxIOSTouchControls) {
      return false;
    }
    std::vector<IOSTouchControlDefinition> controls;
    controls.reserve(controls_array->size());
    size_t control_index = 0;
    for (const auto& control_node : *controls_array) {
      const toml::table* control_table = control_node.as_table();
      if (control_table) {
        controls.push_back(
            DecodeIOSTouchControlDefinition(*control_table, control_index));
        ++control_index;
      }
    }
    if (!ValidateIOSTouchControlDefinitions(controls)) {
      return false;
    }
    layout->controls = std::move(controls);
    return true;
  }

  const toml::table* controls_table = table["controls"].as_table();
  if (!controls_table) {
    return true;
  }

  // Mutate a copy of the seed controls, then swap on validation success.
  // The previous implementation mutated layout->controls in place and only
  // validated after the loop — a malformed override could leave the live
  // layout half-applied and fail validation, leaving partial damage behind.
  std::vector<IOSTouchControlDefinition> controls = layout->controls;
  for (auto& control : controls) {
    const toml::table* control_table =
        (*controls_table)[control.identifier].as_table();
    if (!control_table) {
      continue;
    }
    bool drag_output_overridden = false;
    bool analog_tuning_overridden = false;
    if (auto value = (*control_table)["action"].value<std::string>()) {
      IOSTouchAction parsed_action = control.action;
      if (ParseIOSTouchActionConfigName(*value, &parsed_action) &&
          IsSupportedIOSTouchPrimaryAction(control.type, parsed_action)) {
        ConfigureIOSTouchControlAction(parsed_action, &control);
      }
    }
    ApplyStoredControlLabelState(*control_table, &control);
    if (auto value = (*control_table)["shape"].value<std::string>()) {
      IOSTouchControlShape shape = control.shape;
      if (ParseIOSTouchControlShapeConfigName(*value, &shape)) {
        control.shape = shape;
      }
    }
    if (auto value = (*control_table)["tint_style"].value<std::string>()) {
      IOSTouchTintStyle tint_style = control.tint_style;
      if (ParseIOSTouchTintStyleConfigName(*value, &tint_style)) {
        control.tint_style = tint_style;
      }
    }
    ApplyControlFrame(*control_table, &control.normalized_frame, control.type);
    if (auto value = (*control_table)["has_portrait_frame"].value<bool>()) {
      control.has_portrait_frame = *value;
    }
    if (control.has_portrait_frame) {
      if (const toml::table* portrait_frame_table =
              (*control_table)["portrait_frame"].as_table()) {
        // Seed from the landscape baseline before applying the override so
        // partial portrait_frame blocks don't silently zero an axis.
        control.portrait_normalized_frame = control.normalized_frame;
        ApplyControlFrame(*portrait_frame_table,
                          &control.portrait_normalized_frame, control.type);
      } else {
        control.has_portrait_frame = false;
      }
    }
    if (auto value = (*control_table)["deadzone"].value<double>()) {
      control.deadzone = ClampNormalizedScalar(static_cast<float>(*value));
    }
    if (auto value = (*control_table)["activation_radius"].value<double>()) {
      control.activation_radius =
          ClampPositiveScalar(static_cast<float>(*value), 0.0f, 1.0f);
    }
    if (auto value = (*control_table)["visual_opacity"].value<double>()) {
      control.visual_opacity =
          ClampNormalizedScalar(static_cast<float>(*value));
    }
    if (auto value = (*control_table)["hold_while_captured"].value<bool>()) {
      control.hold_while_captured = *value;
    }
    if (auto value = (*control_table)["enables_relative_look"].value<bool>()) {
      control.enables_relative_look = *value;
    }
    if (auto value = (*control_table)["drag_output"].value<std::string>()) {
      IOSTouchAnalogOutput output = control.drag_output;
      if (ParseIOSTouchAnalogOutputConfigName(*value, &output)) {
        control.drag_output = output;
        drag_output_overridden = true;
      }
    }
    if (const toml::table* tuning_table =
            (*control_table)["analog_tuning"].as_table()) {
      ApplyAnalogTuning(*tuning_table, &control.analog_tuning);
      analog_tuning_overridden = true;
    }
    if (auto value = (*control_table)["relative_look_scale"].value<double>()) {
      control.relative_look_scale =
          ClampPositiveScalar(static_cast<float>(*value), 0.25f, 4.0f);
    }
    if (auto value = (*control_table)["held_look_scale"].value<double>()) {
      control.held_look_scale =
          ClampPositiveScalar(static_cast<float>(*value), 0.25f, 4.0f);
    }
    if (auto value = (*control_table)["held_move_scale"].value<double>()) {
      control.held_move_scale =
          ClampPositiveScalar(static_cast<float>(*value), 0.25f, 4.0f);
    }
    if (auto value = (*control_table)["move_with_dpad_ring"].value<bool>()) {
      control.move_with_dpad_ring = *value;
    }
    if (const toml::table* behavior_table =
            (*control_table)["secondary_behavior"].as_table()) {
      ApplyInteractionBehavior(*behavior_table, &control.secondary_behavior);
    }
    if (auto value = (*control_table)["capture_priority"].value<int64_t>()) {
      control.capture_priority =
          static_cast<uint8_t>(std::clamp<int64_t>(*value, 0, 255));
    }
    FinalizeAnalogControlFromLegacyFields(drag_output_overridden,
                                          analog_tuning_overridden, &control);
    ApplyDerivedTouchControlDefaults(&control);
  }

  if (!ValidateIOSTouchControlDefinitions(controls)) {
    return false;
  }
  layout->controls = std::move(controls);
  return true;
}

bool IsValidIOSTouchLayoutModel(const IOSTouchLayoutModel& layout) {
  return ValidateIOSTouchControlDefinitions(layout.controls);
}

}  // namespace touch
}  // namespace hid
}  // namespace xe
