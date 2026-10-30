/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_TOUCH_TOUCH_INPUT_RESOLVER_H_
#define XENIA_HID_TOUCH_TOUCH_INPUT_RESOLVER_H_

#include <cstdint>

#include "xenia/hid/touch/touch_layout_ios.h"

namespace xe::hid::touch {

enum class IOSTouchComboSubzone : uint8_t {
  kNone = 0,
  kStick,
  kDpadUp,
  kDpadDown,
  kDpadLeft,
  kDpadRight,
};

struct IOSTouchInputCapture {
  IOSTouchPoint anchor_point;
  IOSTouchPoint current_point;
  double began_time = 0.0;
  bool secondary_behavior_triggered = false;
  IOSTouchComboSubzone combo_subzone = IOSTouchComboSubzone::kNone;
};

struct IOSTouchInteractionBehaviorState {
  bool active = false;
  IOSTouchAnalogOutput analog_output = IOSTouchAnalogOutput::kNone;
  IOSTouchAnalogTuning analog_tuning;
  bool enables_relative_look = false;
  float relative_look_scale = 1.0f;
};

IOSTouchPoint ClampTouchLookVector(IOSTouchPoint value);
IOSTouchPoint ApplyTouchAnalogTuning(IOSTouchPoint value,
                                     const IOSTouchAnalogTuning& tuning);
IOSTouchPoint ApplyTouchAnalogTuningWithVelocity(
    IOSTouchPoint value, const IOSTouchAnalogTuning& tuning,
    float velocity_full_scales_per_second);
IOSTouchPoint TouchSwipeLookVectorForDelta(IOSTouchPoint delta,
                                           float look_scale,
                                           float points_per_full_scale,
                                           float vertical_scale);
IOSTouchComboSubzone ResolveTouchComboSubzone(
    const IOSTouchControlDefinition& control,
    const IOSTouchRect& resolved_frame, IOSTouchPoint point);
bool TouchControlContainsPoint(const IOSTouchControlDefinition& control,
                               const IOSTouchRect& resolved_frame,
                               IOSTouchPoint point);
int16_t TouchAxisFromUnit(float unit_value);
bool TouchStatesEqualIgnoringPacket(const IOSTouchResolvedState& left,
                                    const IOSTouchResolvedState& right);
void ApplyTouchActionMapping(const IOSTouchControlDefinition& control,
                             IOSTouchResolvedState* state);
void ApplyTouchActionMappingForAction(IOSTouchAction action,
                                      IOSTouchResolvedState* state);
bool TouchInteractionBehaviorConfigured(
    const IOSTouchInteractionBehavior& behavior);
IOSTouchInteractionBehaviorState ResolveTouchInteractionBehaviorState(
    const IOSTouchInteractionBehavior& behavior,
    const IOSTouchInputCapture& capture, double current_time);
bool TouchControlUsesDeferredPrimaryTap(
    const IOSTouchControlDefinition& control);
IOSTouchPoint MoveStickUnitVectorForCapture(
    const IOSTouchControlDefinition& control, const IOSTouchRect& frame,
    const IOSTouchInputCapture& capture);
bool MoveStickCaptureQualifiesForDoubleTapForward(
    const IOSTouchControlDefinition& control, const IOSTouchRect& frame,
    const IOSTouchInputCapture& capture, double current_time);

}  // namespace xe::hid::touch

#endif  // XENIA_HID_TOUCH_TOUCH_INPUT_RESOLVER_H_
