/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_OVERLAY_GEOMETRY_IOS_H_
#define XENIA_UI_IOS_TOUCH_OVERLAY_GEOMETRY_IOS_H_

#import <UIKit/UIKit.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xenia/hid/touch/touch_layout_ios.h"

namespace xe {
namespace ui {

struct AxisAlignmentSnapResult {
  float origin = 0.0f;
  float guide = 0.0f;
  float snapped_origin = 0.0f;
  bool active = false;
};

struct ScalarSnapResult {
  float value = 0.0f;
  float distance = 0.0f;
  float snapped_value = 0.0f;
  bool active = false;
};

enum class TouchEditGestureMode : uint8_t {
  kMove = 0,
  kResize,
};

struct TouchEditSnapOptions {
  bool grid_enabled = false;
  CGFloat grid_spacing = 0.0f;
  CGFloat move_snap_threshold = 0.0f;
  CGFloat resize_snap_threshold = 0.0f;
  const float* canonical_control_sizes = nullptr;
  size_t canonical_control_size_count = 0;
};

struct TouchEditSnapResult {
  xe::hid::touch::IOSTouchRect frame;
  std::vector<CGFloat> vertical_guides;
  std::vector<CGFloat> horizontal_guides;
};

CGRect CGRectFromTouchRect(const xe::hid::touch::IOSTouchRect& rect);
bool TouchOverlayIsPortraitForView(UIView* view);
xe::hid::touch::IOSTouchLayoutSpace TouchLayoutSpaceForView(UIView* view);
xe::hid::touch::IOSTouchLayoutSpace TouchSafeAreaSpaceForView(UIView* view);
xe::hid::touch::IOSTouchLayoutSpace TouchControlPositionSpaceForControlType(
    UIView* view, xe::hid::touch::IOSTouchControlType control_type);
xe::hid::touch::IOSTouchLayoutSpace TouchControlSizeSpaceForControlType(
    UIView* view, xe::hid::touch::IOSTouchControlType control_type);
xe::hid::touch::IOSTouchRect ClampNormalizedControlFrame(
    const xe::hid::touch::IOSTouchRect& rect, xe::hid::touch::IOSTouchControlType control_type);
xe::hid::touch::IOSTouchRect ResolveNormalizedControlFrame(
    const xe::hid::touch::IOSTouchRect& normalized_frame,
    const xe::hid::touch::IOSTouchLayoutSpace& position_space,
    const xe::hid::touch::IOSTouchLayoutSpace& size_space,
    xe::hid::touch::IOSTouchControlType control_type);
xe::hid::touch::IOSTouchRect ResolveNormalizedControlFrame(
    const xe::hid::touch::IOSTouchRect& normalized_frame,
    const xe::hid::touch::IOSTouchLayoutSpace& safe_area,
    xe::hid::touch::IOSTouchControlType control_type);
xe::hid::touch::IOSTouchRect NormalizedControlFrameFromResolvedFrame(
    const xe::hid::touch::IOSTouchRect& resolved_frame,
    const xe::hid::touch::IOSTouchLayoutSpace& position_space,
    const xe::hid::touch::IOSTouchLayoutSpace& size_space,
    xe::hid::touch::IOSTouchControlType control_type);
xe::hid::touch::IOSTouchRect NormalizedControlFrameFromResolvedFrame(
    const xe::hid::touch::IOSTouchRect& resolved_frame,
    const xe::hid::touch::IOSTouchLayoutSpace& safe_area,
    xe::hid::touch::IOSTouchControlType control_type);
AxisAlignmentSnapResult SoftSnapFrameOrigin(float origin, float extent,
                                            const std::vector<float>& targets, float threshold);
ScalarSnapResult SoftSnapScalar(float value, const std::vector<float>& targets, float threshold);
void AppendUniqueGuidePosition(std::vector<CGFloat>& guides, CGFloat guide);
TouchEditSnapResult SnapTouchEditResolvedFrame(
    size_t control_index, const std::vector<xe::hid::touch::IOSTouchControlDefinition>& controls,
    const std::vector<xe::hid::touch::IOSTouchRect>& resolved_control_frames,
    const xe::hid::touch::IOSTouchRect& candidate_frame,
    const xe::hid::touch::IOSTouchLayoutSpace& position_space,
    const xe::hid::touch::IOSTouchLayoutSpace& size_space, TouchEditGestureMode gesture_mode,
    bool preserve_aspect_ratio, bool preserve_center, const TouchEditSnapOptions& options);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_TOUCH_OVERLAY_GEOMETRY_IOS_H_
