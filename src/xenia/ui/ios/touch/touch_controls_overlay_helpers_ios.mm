/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/touch/touch_controls_overlay_helpers_ios.h"

#include <algorithm>

#include "xenia/base/cvar.h"
#include "xenia/hid/input.h"
#include "xenia/hid/touch/touch_layout_editor.h"
#include "xenia/ui/ios/touch/touch_overlay_style_ios.h"

DEFINE_bool(ios_touch_haptics, true,
            "Play haptic feedback (UIImpactFeedbackGenerator / "
            "UISelectionFeedbackGenerator) on touch button presses, control selection, and snap "
            "engagement. Disable for silent / accessibility setups.",
            "iOS");
DEFINE_bool(ios_touch_overlay, true,
            "Show the on-screen iOS touch controls during gameplay when no hardware controller is "
            "connected. Disable to remove the gameplay overlay while leaving the editor available.",
            "iOS");
DEFINE_double(ios_touch_look_points_per_full_scale, 4.0,
              "Points of swipe motion needed to drive the touch look zone to full stick output. "
              "Lower values make the camera swipe more sensitive.",
              "iOS");
DEFINE_double(ios_touch_look_vertical_scale, 1.20,
              "Extra multiplier for vertical swipe-look motion. Touch aiming often feels slower "
              "vertically because many games apply lower pitch sensitivity than yaw.",
              "iOS");
DEFINE_double(ios_touch_look_hold_seconds, 0.10,
              "How long touch look motion should persist so low-FPS gameplay polls can observe it. "
              "Lower values reduce camera tail; higher values improve capture at low frame rates.",
              "iOS");
DEFINE_double(ios_touch_button_tap_hold_seconds, 0.10,
              "How long tap-style touch buttons stay active so low-FPS gameplay polls can observe "
              "quick presses like jump or reload.",
              "iOS");

namespace xe::ui::ios::touch_overlay {

namespace {

void AppendTouchButtonMaskNames(uint16_t buttons, NSMutableArray<NSString*>* parts) {
  if (!parts) {
    return;
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_A) {
    [parts addObject:@"A"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_B) {
    [parts addObject:@"B"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_X) {
    [parts addObject:@"X"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_Y) {
    [parts addObject:@"Y"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_LEFT_SHOULDER) {
    [parts addObject:@"LB"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_RIGHT_SHOULDER) {
    [parts addObject:@"RB"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_BACK) {
    [parts addObject:@"Back"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_START) {
    [parts addObject:@"Start"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_LEFT_THUMB) {
    [parts addObject:@"LS"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_RIGHT_THUMB) {
    [parts addObject:@"RS"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_DPAD_UP) {
    [parts addObject:@"D-Up"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_DPAD_DOWN) {
    [parts addObject:@"D-Down"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_DPAD_LEFT) {
    [parts addObject:@"D-Left"];
  }
  if (buttons & xe::hid::X_INPUT_GAMEPAD_DPAD_RIGHT) {
    [parts addObject:@"D-Right"];
  }
}

NSString* TouchInteractionBehaviorOutputText(
    const xe::hid::touch::IOSTouchInteractionBehavior& behavior) {
  NSMutableArray<NSString*>* outputs = [NSMutableArray array];
  if (behavior.action != xe::hid::touch::IOSTouchAction::kNone) {
    [outputs addObject:[NSString stringWithUTF8String:xe::hid::touch::IOSTouchActionDisplayName(
                                                          behavior.action)]];
  }
  xe::hid::touch::IOSTouchAnalogOutput analog_output = behavior.analog_output;
  if (analog_output == xe::hid::touch::IOSTouchAnalogOutput::kNone &&
      behavior.enables_relative_look) {
    analog_output = xe::hid::touch::IOSTouchAnalogOutput::kLook;
  }
  if (analog_output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
    [outputs addObject:[NSString stringWithUTF8String:
                                     xe::hid::touch::IOSTouchAnalogOutputDisplayName(
                                         analog_output)]];
  }
  return outputs.count ? [outputs componentsJoinedByString:@" + "] : @"Unused";
}

xe::hid::touch::IOSTouchPoint ToInputPoint(CGPoint point) {
  return xe::hid::touch::IOSTouchPoint{static_cast<float>(point.x), static_cast<float>(point.y)};
}

CGPoint ToCGPoint(xe::hid::touch::IOSTouchPoint point) { return CGPointMake(point.x, point.y); }

xe::hid::touch::IOSTouchComboSubzone ToInputComboSubzone(TouchCaptureState::ComboSubzone subzone) {
  switch (subzone) {
    case TouchCaptureState::ComboSubzone::kStick:
      return xe::hid::touch::IOSTouchComboSubzone::kStick;
    case TouchCaptureState::ComboSubzone::kDpadUp:
      return xe::hid::touch::IOSTouchComboSubzone::kDpadUp;
    case TouchCaptureState::ComboSubzone::kDpadDown:
      return xe::hid::touch::IOSTouchComboSubzone::kDpadDown;
    case TouchCaptureState::ComboSubzone::kDpadLeft:
      return xe::hid::touch::IOSTouchComboSubzone::kDpadLeft;
    case TouchCaptureState::ComboSubzone::kDpadRight:
      return xe::hid::touch::IOSTouchComboSubzone::kDpadRight;
    case TouchCaptureState::ComboSubzone::kNone:
    default:
      return xe::hid::touch::IOSTouchComboSubzone::kNone;
  }
}

TouchCaptureState::ComboSubzone ToOverlayComboSubzone(
    xe::hid::touch::IOSTouchComboSubzone subzone) {
  switch (subzone) {
    case xe::hid::touch::IOSTouchComboSubzone::kStick:
      return TouchCaptureState::ComboSubzone::kStick;
    case xe::hid::touch::IOSTouchComboSubzone::kDpadUp:
      return TouchCaptureState::ComboSubzone::kDpadUp;
    case xe::hid::touch::IOSTouchComboSubzone::kDpadDown:
      return TouchCaptureState::ComboSubzone::kDpadDown;
    case xe::hid::touch::IOSTouchComboSubzone::kDpadLeft:
      return TouchCaptureState::ComboSubzone::kDpadLeft;
    case xe::hid::touch::IOSTouchComboSubzone::kDpadRight:
      return TouchCaptureState::ComboSubzone::kDpadRight;
    case xe::hid::touch::IOSTouchComboSubzone::kNone:
    default:
      return TouchCaptureState::ComboSubzone::kNone;
  }
}

xe::hid::touch::IOSTouchInputCapture ToInputCapture(const TouchCaptureState& capture) {
  xe::hid::touch::IOSTouchInputCapture input_capture;
  input_capture.anchor_point = ToInputPoint(capture.anchor_point);
  input_capture.current_point = ToInputPoint(capture.current_point);
  input_capture.began_time = capture.began_time;
  input_capture.secondary_behavior_triggered = capture.secondary_behavior_triggered;
  input_capture.combo_subzone = ToInputComboSubzone(capture.combo_subzone);
  return input_capture;
}

}  // namespace

const float kEditCanonicalControlSizes[9] = {
    0.08f, 0.10f, 0.12f, 0.14f, 0.16f, 0.18f, 0.20f, 0.24f, 0.28f,
};

const float kLookZoneScaleChoices[8] = {
    0.25f, 0.50f, 0.75f, 1.00f, 1.50f, 2.00f, 3.00f, 4.00f,
};

const float kRelativeLookScaleChoices[8] = {
    0.50f, 0.66f, 0.80f, 0.92f, 1.05f, 1.25f, 1.50f, 2.00f,
};

const float kTouchAnalogScaleChoices[9] = {
    0.25f, 0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 2.00f, 3.00f, 4.00f,
};

const float kTouchAnalogPercentChoices[8] = {
    0.00f, 0.05f, 0.10f, 0.14f, 0.18f, 0.24f, 0.32f, 0.48f,
};

const float kTouchAnalogRadiusChoices[7] = {
    0.24f, 0.33f, 0.40f, 0.48f, 0.56f, 0.70f, 1.00f,
};

const float kTouchAnalogCurveChoices[7] = {
    0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 2.00f, 3.00f,
};

const float kTouchAnalogAccelerationChoices[6] = {
    0.00f, 0.25f, 0.50f, 0.75f, 1.00f, 2.00f,
};

const float kTouchAnalogSmoothingChoices[6] = {
    0.00f, 0.10f, 0.20f, 0.35f, 0.55f, 0.80f,
};

const float kTouchAnalogMaxOutputChoices[5] = {
    0.25f, 0.50f, 0.75f, 0.90f, 1.00f,
};

const xe::hid::touch::IOSTouchControlShape kEditShapeChoices[2] = {
    xe::hid::touch::IOSTouchControlShape::kCircle,
    xe::hid::touch::IOSTouchControlShape::kRoundedRect,
};

float TouchLookPointsPerFullScale() {
  return std::clamp(static_cast<float>(::cvars::ios_touch_look_points_per_full_scale), 1.0f, 64.0f);
}

float TouchLookVerticalScale() {
  return std::clamp(static_cast<float>(::cvars::ios_touch_look_vertical_scale), 0.25f, 4.0f);
}

float TouchLookHoldSeconds() {
  return std::clamp(static_cast<float>(::cvars::ios_touch_look_hold_seconds), 0.016f, 0.25f);
}

float TouchButtonTapHoldSeconds() {
  return std::clamp(static_cast<float>(::cvars::ios_touch_button_tap_hold_seconds), 0.016f, 0.25f);
}

CGPoint ClampLookVector(CGPoint value) {
  return ToCGPoint(xe::hid::touch::ClampTouchLookVector(ToInputPoint(value)));
}

CGPoint SwipeLookVectorForDelta(CGPoint delta, float look_scale) {
  return ToCGPoint(xe::hid::touch::TouchSwipeLookVectorForDelta(
      ToInputPoint(delta), look_scale, TouchLookPointsPerFullScale(), TouchLookVerticalScale()));
}

CGPoint ApplyTouchAnalogTuning(CGPoint value,
                               const xe::hid::touch::IOSTouchAnalogTuning& tuning) {
  return ToCGPoint(xe::hid::touch::ApplyTouchAnalogTuning(ToInputPoint(value), tuning));
}

CGPoint ApplyTouchAnalogTuningWithVelocity(
    CGPoint value, const xe::hid::touch::IOSTouchAnalogTuning& tuning,
    float velocity_full_scales_per_second) {
  return ToCGPoint(xe::hid::touch::ApplyTouchAnalogTuningWithVelocity(
      ToInputPoint(value), tuning, velocity_full_scales_per_second));
}

std::array<CGRect, kEditChromeDockCount> EditChromeDockCandidateFrames(
    const xe::hid::touch::IOSTouchLayoutSpace& safe_area, CGFloat chrome_margin,
    CGFloat chrome_width, CGFloat chrome_height) {
  const CGFloat min_x = safe_area.origin_x + chrome_margin;
  const CGFloat max_x =
      MAX(min_x, safe_area.origin_x + safe_area.width - chrome_width - chrome_margin);
  const CGFloat min_y = safe_area.origin_y + chrome_margin;
  const CGFloat max_y =
      MAX(min_y, safe_area.origin_y + safe_area.height - chrome_height - chrome_margin);
  const CGFloat center_x =
      std::clamp(safe_area.origin_x + (safe_area.width - chrome_width) * 0.5f, min_x, max_x);
  const CGFloat center_y =
      std::clamp(safe_area.origin_y + (safe_area.height - chrome_height) * 0.5f, min_y, max_y);
  return {
      CGRectMake(min_x, min_y, chrome_width, chrome_height),
      CGRectMake(max_x, min_y, chrome_width, chrome_height),
      CGRectMake(min_x, max_y, chrome_width, chrome_height),
      CGRectMake(max_x, max_y, chrome_width, chrome_height),
      CGRectMake(center_x, min_y, chrome_width, chrome_height),
      CGRectMake(center_x, max_y, chrome_width, chrome_height),
      CGRectMake(min_x, center_y, chrome_width, chrome_height),
      CGRectMake(max_x, center_y, chrome_width, chrome_height),
  };
}

TouchCaptureState::ComboSubzone TouchComboSubzoneForPoint(
    const xe::hid::touch::IOSTouchControlDefinition& control,
    const xe::hid::touch::IOSTouchRect& resolved_frame, CGPoint point) {
  return ToOverlayComboSubzone(
      xe::hid::touch::ResolveTouchComboSubzone(control, resolved_frame, ToInputPoint(point)));
}

bool TouchControlContainsPoint(const xe::hid::touch::IOSTouchControlDefinition& control,
                               const xe::hid::touch::IOSTouchRect& resolved_frame, CGPoint point) {
  return xe::hid::touch::TouchControlContainsPoint(control, resolved_frame, ToInputPoint(point));
}

TouchInteractionBehaviorState ResolveTouchInteractionBehaviorState(
    const xe::hid::touch::IOSTouchInteractionBehavior& behavior, const TouchCaptureState& capture,
    CFTimeInterval current_time) {
  return xe::hid::touch::ResolveTouchInteractionBehaviorState(behavior, ToInputCapture(capture),
                                                              current_time);
}

NSString* TouchInteractionBehaviorSummaryText(
    const xe::hid::touch::IOSTouchInteractionBehavior& behavior) {
  if (behavior.trigger == xe::hid::touch::IOSTouchInteractionTrigger::kNone) {
    return @"Behavior: Off";
  }
  NSString* trigger = [NSString
      stringWithUTF8String:xe::hid::touch::IOSTouchInteractionTriggerDisplayName(behavior.trigger)];
  NSString* outputs = TouchInteractionBehaviorOutputText(behavior);
  return [NSString stringWithFormat:@"Behavior: %@ -> %@", trigger, outputs];
}

CGPoint MoveStickUnitVectorForCapture(const xe::hid::touch::IOSTouchControlDefinition& control,
                                      const xe::hid::touch::IOSTouchRect& frame,
                                      const TouchCaptureState& capture) {
  return ToCGPoint(
      xe::hid::touch::MoveStickUnitVectorForCapture(control, frame, ToInputCapture(capture)));
}

bool MoveStickCaptureQualifiesForDoubleTapForward(
    const xe::hid::touch::IOSTouchControlDefinition& control,
    const xe::hid::touch::IOSTouchRect& frame, const TouchCaptureState& capture,
    CFTimeInterval current_time) {
  return xe::hid::touch::MoveStickCaptureQualifiesForDoubleTapForward(
      control, frame, ToInputCapture(capture), current_time);
}

NSString* TouchButtonMaskPreviewText(uint16_t buttons) {
  NSMutableArray<NSString*>* parts = [NSMutableArray array];
  AppendTouchButtonMaskNames(buttons, parts);
  return parts.count ? [parts componentsJoinedByString:@" + "] : @"0";
}

NSString* TouchControlShapeDisplayText(xe::hid::touch::IOSTouchControlShape shape) {
  switch (shape) {
    case xe::hid::touch::IOSTouchControlShape::kCircle:
      return @"Circle";
    case xe::hid::touch::IOSTouchControlShape::kRoundedRect:
      return @"Rounded Rectangle";
  }
  return @"Round";
}

CGFloat TouchEditPreviewHeightForLabel(UILabel* label, CGFloat width) {
  if (label.hidden || width <= 0.0f) {
    return 0.0f;
  }
  CGSize measured = [label sizeThatFits:CGSizeMake(width, CGFLOAT_MAX)];
  return MAX(34.0f, ceil(measured.height) + 12.0f);
}

CGFloat LayoutVisibleButtonsRow(CGFloat y, CGFloat x, CGFloat width, CGFloat height, CGFloat gap,
                                NSArray<UIButton*>* buttons) {
  NSMutableArray<UIButton*>* visible_buttons = [NSMutableArray arrayWithCapacity:buttons.count];
  for (UIButton* button in buttons) {
    if (button.hidden) {
      button.frame = CGRectZero;
      continue;
    }
    [visible_buttons addObject:button];
  }
  if (!visible_buttons.count) {
    return y;
  }

  const CGFloat available_width = width - gap * (visible_buttons.count - 1);
  const CGFloat base_width = floor(available_width / visible_buttons.count);
  CGFloat current_x = x;
  for (NSUInteger button_index = 0; button_index < visible_buttons.count; ++button_index) {
    UIButton* button = [visible_buttons objectAtIndex:button_index];
    const CGFloat button_width =
        button_index + 1 == visible_buttons.count ? x + width - current_x : base_width;
    button.frame = CGRectMake(current_x, y, button_width, height);
    current_x += button_width + gap;
  }
  return y + height + gap;
}

}  // namespace xe::ui::ios::touch_overlay
