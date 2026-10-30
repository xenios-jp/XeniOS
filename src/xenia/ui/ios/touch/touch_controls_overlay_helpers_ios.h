/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_CONTROLS_OVERLAY_HELPERS_IOS_H_
#define XENIA_UI_IOS_TOUCH_CONTROLS_OVERLAY_HELPERS_IOS_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "xenia/hid/touch/touch_input_resolver.h"
#include "xenia/hid/touch/touch_layout_ios.h"

namespace xe::ui::ios::touch_overlay {

inline constexpr float kTouchAxisMax = 32767.0f;
inline constexpr CGFloat kEditGridSpacingPoints = 28.0f;
inline constexpr CGFloat kEditGridDotRadius = 1.55f;
inline constexpr CGFloat kEditMoveSnapThresholdPoints = 22.0f;
inline constexpr CGFloat kEditResizeSnapThresholdPoints = 18.0f;
inline constexpr CGFloat kEditGridMoveSnapThresholdPoints = 38.0f;
inline constexpr CGFloat kEditGridResizeSnapThresholdPoints = 30.0f;
inline constexpr CGFloat kEditSnapGuideLineWidth = 1.75f;
inline constexpr CGFloat kEditChromeHeaderHeight = 44.0f;
inline constexpr NSInteger kEditChromeDockCount = 8;

extern const float kEditCanonicalControlSizes[9];
extern const float kLookZoneScaleChoices[8];
extern const float kRelativeLookScaleChoices[8];
extern const float kTouchAnalogScaleChoices[9];
extern const float kTouchAnalogPercentChoices[8];
extern const float kTouchAnalogRadiusChoices[7];
extern const float kTouchAnalogCurveChoices[7];
extern const float kTouchAnalogAccelerationChoices[6];
extern const float kTouchAnalogSmoothingChoices[6];
extern const float kTouchAnalogMaxOutputChoices[5];
extern const xe::hid::touch::IOSTouchControlShape kEditShapeChoices[2];

struct TouchCaptureState {
  enum class EditGestureMode : uint8_t {
    kMove = 0,
    kResize,
  };

  enum class ComboSubzone : uint8_t {
    kNone = 0,
    kStick,
    kDpadUp,
    kDpadDown,
    kDpadLeft,
    kDpadRight,
  };

  UITouch* touch = nil;
  NSUInteger control_index = NSNotFound;
  CGPoint anchor_point = CGPointZero;
  CGPoint current_point = CGPointZero;
  CFTimeInterval began_time = 0.0;
  CFTimeInterval last_motion_time = 0.0;
  bool secondary_behavior_triggered = false;
  xe::hid::touch::IOSTouchRect normalized_frame_at_capture;
  EditGestureMode edit_gesture_mode = EditGestureMode::kMove;
  ComboSubzone combo_subzone = ComboSubzone::kNone;
};

using TouchInteractionBehaviorState = xe::hid::touch::IOSTouchInteractionBehaviorState;

float TouchLookPointsPerFullScale();
float TouchLookVerticalScale();
float TouchLookHoldSeconds();
float TouchButtonTapHoldSeconds();
CGPoint ClampLookVector(CGPoint value);
CGPoint SwipeLookVectorForDelta(CGPoint delta, float look_scale);
CGPoint ApplyTouchAnalogTuning(CGPoint value, const xe::hid::touch::IOSTouchAnalogTuning& tuning);
CGPoint ApplyTouchAnalogTuningWithVelocity(CGPoint value,
                                           const xe::hid::touch::IOSTouchAnalogTuning& tuning,
                                           float velocity_full_scales_per_second);

std::array<CGRect, kEditChromeDockCount> EditChromeDockCandidateFrames(
    const xe::hid::touch::IOSTouchLayoutSpace& safe_area, CGFloat chrome_margin,
    CGFloat chrome_width, CGFloat chrome_height);
TouchCaptureState::ComboSubzone TouchComboSubzoneForPoint(
    const xe::hid::touch::IOSTouchControlDefinition& control,
    const xe::hid::touch::IOSTouchRect& resolved_frame, CGPoint point);
bool TouchControlContainsPoint(const xe::hid::touch::IOSTouchControlDefinition& control,
                               const xe::hid::touch::IOSTouchRect& resolved_frame, CGPoint point);
TouchInteractionBehaviorState ResolveTouchInteractionBehaviorState(
    const xe::hid::touch::IOSTouchInteractionBehavior& behavior, const TouchCaptureState& capture,
    CFTimeInterval current_time);
NSString* TouchInteractionBehaviorSummaryText(
    const xe::hid::touch::IOSTouchInteractionBehavior& behavior);
CGPoint MoveStickUnitVectorForCapture(const xe::hid::touch::IOSTouchControlDefinition& control,
                                      const xe::hid::touch::IOSTouchRect& frame,
                                      const TouchCaptureState& capture);
bool MoveStickCaptureQualifiesForDoubleTapForward(
    const xe::hid::touch::IOSTouchControlDefinition& control,
    const xe::hid::touch::IOSTouchRect& frame, const TouchCaptureState& capture,
    CFTimeInterval current_time);
NSString* TouchButtonMaskPreviewText(uint16_t buttons);
NSString* TouchControlShapeDisplayText(xe::hid::touch::IOSTouchControlShape shape);
CGFloat TouchEditPreviewHeightForLabel(UILabel* label, CGFloat width);
CGFloat LayoutVisibleButtonsRow(CGFloat y, CGFloat x, CGFloat width, CGFloat height, CGFloat gap,
                                NSArray<UIButton*>* buttons);

}  // namespace xe::ui::ios::touch_overlay

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_TOUCH_CONTROLS_OVERLAY_HELPERS_IOS_H_
