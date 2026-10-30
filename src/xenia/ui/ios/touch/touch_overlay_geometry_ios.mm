/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/touch/touch_overlay_geometry_ios.h"

#include <algorithm>
#include <cmath>

namespace xe {
namespace ui {
namespace {

float MaxNormalizedControlSizeForType(xe::hid::touch::IOSTouchControlType control_type) {
  return control_type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone ? 1.0f : 0.98f;
}

xe::hid::touch::IOSTouchRect ClampNormalizedControlFrameForSpaces(
    const xe::hid::touch::IOSTouchRect& rect,
    const xe::hid::touch::IOSTouchLayoutSpace& position_space,
    const xe::hid::touch::IOSTouchLayoutSpace& size_space,
    xe::hid::touch::IOSTouchControlType control_type) {
  if (position_space.IsEmpty() || size_space.IsEmpty()) {
    return {};
  }

  xe::hid::touch::IOSTouchRect result = rect;
  const float max_control_size = MaxNormalizedControlSizeForType(control_type);
  result.width = std::clamp(result.width, 0.05f, max_control_size);
  result.height = std::clamp(result.height, 0.05f, max_control_size);
  const float width_in_position_space = result.width * size_space.width / position_space.width;
  const float height_in_position_space = result.height * size_space.height / position_space.height;
  result.x = std::clamp(result.x, 0.0f, std::max(0.0f, 1.0f - width_in_position_space));
  result.y = std::clamp(result.y, 0.0f, std::max(0.0f, 1.0f - height_in_position_space));
  return result;
}

float SoftSnapBlend(float distance, float threshold) {
  if (threshold <= 0.0f) {
    return 0.0f;
  }
  const float t = std::clamp(1.0f - distance / threshold, 0.0f, 1.0f);
  return 0.28f + 0.68f * t * t;
}

}  // namespace

CGRect CGRectFromTouchRect(const xe::hid::touch::IOSTouchRect& rect) {
  return CGRectMake(rect.x, rect.y, rect.width, rect.height);
}

bool TouchOverlayIsPortraitForView(UIView* view) {
  if (!view) {
    return false;
  }
  CGSize size = view.bounds.size;
  return size.height >= size.width;
}

xe::hid::touch::IOSTouchLayoutSpace TouchLayoutSpaceForView(UIView* view) {
  if (!view) {
    return {};
  }

  CGRect bounds = view.bounds;
  return xe::hid::touch::IOSTouchLayoutSpace{
      0.0f,
      0.0f,
      static_cast<float>(MAX(bounds.size.width, 0.0)),
      static_cast<float>(MAX(bounds.size.height, 0.0)),
  };
}

xe::hid::touch::IOSTouchLayoutSpace TouchSafeAreaSpaceForView(UIView* view) {
  if (!view) {
    return {};
  }

  UIEdgeInsets insets = view.safeAreaInsets;
  CGRect bounds = view.bounds;
  return xe::hid::touch::IOSTouchLayoutSpace{
      static_cast<float>(insets.left),
      static_cast<float>(insets.top),
      static_cast<float>(MAX(bounds.size.width - insets.left - insets.right, 0.0)),
      static_cast<float>(MAX(bounds.size.height - insets.top - insets.bottom, 0.0)),
  };
}

xe::hid::touch::IOSTouchLayoutSpace TouchControlPositionSpaceForControlType(
    UIView* view, xe::hid::touch::IOSTouchControlType control_type) {
  static_cast<void>(control_type);
  return TouchLayoutSpaceForView(view);
}

xe::hid::touch::IOSTouchLayoutSpace TouchControlSizeSpaceForControlType(
    UIView* view, xe::hid::touch::IOSTouchControlType control_type) {
  static_cast<void>(control_type);
  return TouchLayoutSpaceForView(view);
}

xe::hid::touch::IOSTouchRect ClampNormalizedControlFrame(
    const xe::hid::touch::IOSTouchRect& rect, xe::hid::touch::IOSTouchControlType control_type) {
  xe::hid::touch::IOSTouchRect result = rect;
  const float max_control_size = MaxNormalizedControlSizeForType(control_type);
  result.width = std::clamp(result.width, 0.05f, max_control_size);
  result.height = std::clamp(result.height, 0.05f, max_control_size);
  result.x = std::clamp(result.x, 0.0f, 1.0f - result.width);
  result.y = std::clamp(result.y, 0.0f, 1.0f - result.height);
  return result;
}

xe::hid::touch::IOSTouchRect ResolveNormalizedControlFrame(
    const xe::hid::touch::IOSTouchRect& normalized_frame,
    const xe::hid::touch::IOSTouchLayoutSpace& position_space,
    const xe::hid::touch::IOSTouchLayoutSpace& size_space,
    xe::hid::touch::IOSTouchControlType control_type) {
  xe::hid::touch::IOSTouchRect clamped_frame = ClampNormalizedControlFrameForSpaces(
      normalized_frame, position_space, size_space, control_type);
  if (position_space.IsEmpty() || size_space.IsEmpty()) {
    return {};
  }
  return xe::hid::touch::IOSTouchRect{
      position_space.origin_x + clamped_frame.x * position_space.width,
      position_space.origin_y + clamped_frame.y * position_space.height,
      clamped_frame.width * size_space.width,
      clamped_frame.height * size_space.height,
  };
}

xe::hid::touch::IOSTouchRect ResolveNormalizedControlFrame(
    const xe::hid::touch::IOSTouchRect& normalized_frame,
    const xe::hid::touch::IOSTouchLayoutSpace& safe_area,
    xe::hid::touch::IOSTouchControlType control_type) {
  return xe::hid::touch::ResolveIOSTouchRect(
      ClampNormalizedControlFrame(normalized_frame, control_type), safe_area);
}

xe::hid::touch::IOSTouchRect NormalizedControlFrameFromResolvedFrame(
    const xe::hid::touch::IOSTouchRect& resolved_frame,
    const xe::hid::touch::IOSTouchLayoutSpace& position_space,
    const xe::hid::touch::IOSTouchLayoutSpace& size_space,
    xe::hid::touch::IOSTouchControlType control_type) {
  if (position_space.IsEmpty() || size_space.IsEmpty()) {
    return {};
  }

  return ClampNormalizedControlFrameForSpaces(
      xe::hid::touch::IOSTouchRect{
          (resolved_frame.x - position_space.origin_x) / position_space.width,
          (resolved_frame.y - position_space.origin_y) / position_space.height,
          resolved_frame.width / size_space.width,
          resolved_frame.height / size_space.height,
      },
      position_space, size_space, control_type);
}

xe::hid::touch::IOSTouchRect NormalizedControlFrameFromResolvedFrame(
    const xe::hid::touch::IOSTouchRect& resolved_frame,
    const xe::hid::touch::IOSTouchLayoutSpace& safe_area,
    xe::hid::touch::IOSTouchControlType control_type) {
  if (safe_area.width <= 0.0f || safe_area.height <= 0.0f) {
    return {};
  }

  return ClampNormalizedControlFrame(
      xe::hid::touch::IOSTouchRect{
          (resolved_frame.x - safe_area.origin_x) / safe_area.width,
          (resolved_frame.y - safe_area.origin_y) / safe_area.height,
          resolved_frame.width / safe_area.width,
          resolved_frame.height / safe_area.height,
      },
      control_type);
}

AxisAlignmentSnapResult SoftSnapFrameOrigin(float origin, float extent,
                                            const std::vector<float>& targets, float threshold) {
  AxisAlignmentSnapResult best;
  best.origin = origin;
  float best_distance = threshold + 1.0f;
  float best_snapped_origin = origin;
  float best_guide = 0.0f;

  auto consider = [&](float snapped_origin, float guide) {
    const float distance = std::abs(snapped_origin - origin);
    if (distance < best_distance) {
      best_distance = distance;
      best_snapped_origin = snapped_origin;
      best_guide = guide;
    }
  };

  for (float target : targets) {
    consider(target, target);
    consider(target - extent * 0.5f, target);
    consider(target - extent, target);
  }

  if (best_distance > threshold) {
    return best;
  }

  best.active = true;
  best.guide = best_guide;
  best.snapped_origin = best_snapped_origin;
  best.origin =
      best_distance <= threshold * 0.20f
          ? best_snapped_origin
          : origin + (best_snapped_origin - origin) * SoftSnapBlend(best_distance, threshold);
  return best;
}

ScalarSnapResult SoftSnapScalar(float value, const std::vector<float>& targets, float threshold) {
  ScalarSnapResult result;
  result.value = value;
  result.distance = threshold + 1.0f;
  float best_target = value;
  for (float target : targets) {
    const float distance = std::abs(target - value);
    if (distance < result.distance) {
      result.distance = distance;
      best_target = target;
    }
  }

  if (result.distance > threshold) {
    result.distance = 0.0f;
    return result;
  }

  result.active = true;
  result.snapped_value = best_target;
  result.value = result.distance <= threshold * 0.20f
                     ? best_target
                     : value + (best_target - value) * SoftSnapBlend(result.distance, threshold);
  return result;
}

void AppendUniqueGuidePosition(std::vector<CGFloat>& guides, CGFloat guide) {
  const bool exists = std::any_of(guides.begin(), guides.end(), [guide](CGFloat existing) {
    return std::abs(existing - guide) < 0.5f;
  });
  if (!exists) {
    guides.push_back(guide);
  }
}

namespace {

bool TouchEditShouldSkipPeerControl(
    const std::vector<xe::hid::touch::IOSTouchControlDefinition>& controls, size_t candidate_index,
    size_t control_index) {
  return candidate_index == control_index ||
         controls[candidate_index].type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone;
}

std::vector<float> TouchEditVerticalAlignmentTargets(
    size_t control_index, const std::vector<xe::hid::touch::IOSTouchControlDefinition>& controls,
    const std::vector<xe::hid::touch::IOSTouchRect>& resolved_control_frames,
    const xe::hid::touch::IOSTouchLayoutSpace& safe_area, const TouchEditSnapOptions& options) {
  std::vector<float> targets = {safe_area.origin_x, safe_area.origin_x + safe_area.width * 0.5f,
                                safe_area.origin_x + safe_area.width};
  if (options.grid_enabled && options.grid_spacing > 0.0f) {
    for (float x = safe_area.origin_x + options.grid_spacing * 0.5f;
         x < safe_area.origin_x + safe_area.width; x += options.grid_spacing) {
      targets.push_back(x);
    }
  }

  const size_t control_count = std::min(resolved_control_frames.size(), controls.size());
  for (size_t index = 0; index < control_count; ++index) {
    if (TouchEditShouldSkipPeerControl(controls, index, control_index)) {
      continue;
    }
    const xe::hid::touch::IOSTouchRect& frame = resolved_control_frames[index];
    targets.push_back(frame.x);
    targets.push_back(frame.x + frame.width * 0.5f);
    targets.push_back(frame.x + frame.width);
  }
  return targets;
}

std::vector<float> TouchEditHorizontalAlignmentTargets(
    size_t control_index, const std::vector<xe::hid::touch::IOSTouchControlDefinition>& controls,
    const std::vector<xe::hid::touch::IOSTouchRect>& resolved_control_frames,
    const xe::hid::touch::IOSTouchLayoutSpace& safe_area, const TouchEditSnapOptions& options) {
  std::vector<float> targets = {safe_area.origin_y, safe_area.origin_y + safe_area.height * 0.5f,
                                safe_area.origin_y + safe_area.height};
  if (options.grid_enabled && options.grid_spacing > 0.0f) {
    for (float y = safe_area.origin_y + options.grid_spacing * 0.5f;
         y < safe_area.origin_y + safe_area.height; y += options.grid_spacing) {
      targets.push_back(y);
    }
  }

  const size_t control_count = std::min(resolved_control_frames.size(), controls.size());
  for (size_t index = 0; index < control_count; ++index) {
    if (TouchEditShouldSkipPeerControl(controls, index, control_index)) {
      continue;
    }
    const xe::hid::touch::IOSTouchRect& frame = resolved_control_frames[index];
    targets.push_back(frame.y);
    targets.push_back(frame.y + frame.height * 0.5f);
    targets.push_back(frame.y + frame.height);
  }
  return targets;
}

std::vector<float> TouchEditWidthSnapTargets(
    size_t control_index, const std::vector<xe::hid::touch::IOSTouchControlDefinition>& controls,
    const std::vector<xe::hid::touch::IOSTouchRect>& resolved_control_frames,
    const xe::hid::touch::IOSTouchLayoutSpace& size_space, const TouchEditSnapOptions& options) {
  std::vector<float> targets;
  const size_t grid_target_count =
      options.grid_spacing > 0.0f ? static_cast<size_t>(size_space.width / options.grid_spacing) + 1
                                  : 0;
  targets.reserve(options.canonical_control_size_count + resolved_control_frames.size() +
                  grid_target_count);
  if (options.grid_enabled && options.grid_spacing > 0.0f) {
    for (float width = options.grid_spacing; width <= size_space.width;
         width += options.grid_spacing) {
      targets.push_back(width);
    }
  }
  for (size_t index = 0; index < options.canonical_control_size_count; ++index) {
    targets.push_back(size_space.width * options.canonical_control_sizes[index]);
  }

  if (control_index < controls.size() &&
      controls[control_index].type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
    targets.push_back(size_space.width);
  }
  const size_t control_count = std::min(resolved_control_frames.size(), controls.size());
  for (size_t index = 0; index < control_count; ++index) {
    if (TouchEditShouldSkipPeerControl(controls, index, control_index)) {
      continue;
    }
    targets.push_back(resolved_control_frames[index].width);
  }
  return targets;
}

std::vector<float> TouchEditHeightSnapTargets(
    size_t control_index, const std::vector<xe::hid::touch::IOSTouchControlDefinition>& controls,
    const std::vector<xe::hid::touch::IOSTouchRect>& resolved_control_frames,
    const xe::hid::touch::IOSTouchLayoutSpace& size_space, const TouchEditSnapOptions& options) {
  std::vector<float> targets;
  const size_t grid_target_count =
      options.grid_spacing > 0.0f
          ? static_cast<size_t>(size_space.height / options.grid_spacing) + 1
          : 0;
  targets.reserve(options.canonical_control_size_count + resolved_control_frames.size() +
                  grid_target_count);
  if (options.grid_enabled && options.grid_spacing > 0.0f) {
    for (float height = options.grid_spacing; height <= size_space.height;
         height += options.grid_spacing) {
      targets.push_back(height);
    }
  }
  for (size_t index = 0; index < options.canonical_control_size_count; ++index) {
    targets.push_back(size_space.height * options.canonical_control_sizes[index]);
  }

  if (control_index < controls.size() &&
      controls[control_index].type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
    targets.push_back(size_space.height);
  }
  const size_t control_count = std::min(resolved_control_frames.size(), controls.size());
  for (size_t index = 0; index < control_count; ++index) {
    if (TouchEditShouldSkipPeerControl(controls, index, control_index)) {
      continue;
    }
    targets.push_back(resolved_control_frames[index].height);
  }
  return targets;
}

}  // namespace

TouchEditSnapResult SnapTouchEditResolvedFrame(
    size_t control_index, const std::vector<xe::hid::touch::IOSTouchControlDefinition>& controls,
    const std::vector<xe::hid::touch::IOSTouchRect>& resolved_control_frames,
    const xe::hid::touch::IOSTouchRect& candidate_frame,
    const xe::hid::touch::IOSTouchLayoutSpace& position_space,
    const xe::hid::touch::IOSTouchLayoutSpace& size_space, TouchEditGestureMode gesture_mode,
    bool preserve_aspect_ratio, bool preserve_center, const TouchEditSnapOptions& options) {
  TouchEditSnapResult result;
  result.frame = candidate_frame;
  if (control_index >= controls.size() || position_space.IsEmpty() || size_space.IsEmpty()) {
    return result;
  }

  const auto& control = controls[control_index];
  xe::hid::touch::IOSTouchRect frame =
      ResolveNormalizedControlFrame(NormalizedControlFrameFromResolvedFrame(
                                        candidate_frame, position_space, size_space, control.type),
                                    position_space, size_space, control.type);
  if (!options.grid_enabled) {
    result.frame = frame;
    return result;
  }

  if (gesture_mode == TouchEditGestureMode::kMove) {
    const CGFloat move_threshold = options.move_snap_threshold;
    AxisAlignmentSnapResult vertical_result = SoftSnapFrameOrigin(
        frame.x, frame.width,
        TouchEditVerticalAlignmentTargets(control_index, controls, resolved_control_frames,
                                          position_space, options),
        move_threshold);
    AxisAlignmentSnapResult horizontal_result = SoftSnapFrameOrigin(
        frame.y, frame.height,
        TouchEditHorizontalAlignmentTargets(control_index, controls, resolved_control_frames,
                                            position_space, options),
        move_threshold);
    frame.x = options.grid_enabled && vertical_result.active ? vertical_result.snapped_origin
                                                             : vertical_result.origin;
    frame.y = options.grid_enabled && horizontal_result.active ? horizontal_result.snapped_origin
                                                               : horizontal_result.origin;
    if (vertical_result.active) {
      AppendUniqueGuidePosition(result.vertical_guides, vertical_result.guide);
    }
    if (horizontal_result.active) {
      AppendUniqueGuidePosition(result.horizontal_guides, horizontal_result.guide);
    }
  } else {
    const CGFloat resize_threshold = options.resize_snap_threshold;
    ScalarSnapResult width_result =
        SoftSnapScalar(frame.width,
                       TouchEditWidthSnapTargets(control_index, controls, resolved_control_frames,
                                                 size_space, options),
                       resize_threshold);
    ScalarSnapResult height_result =
        SoftSnapScalar(frame.height,
                       TouchEditHeightSnapTargets(control_index, controls, resolved_control_frames,
                                                  size_space, options),
                       resize_threshold);
    if (preserve_aspect_ratio) {
      const float origin_x = frame.x;
      const float origin_y = frame.y;
      const float center_x = frame.x + frame.width * 0.5f;
      const float center_y = frame.y + frame.height * 0.5f;
      const float aspect_ratio =
          frame.width > 0.0f ? std::max(frame.height / frame.width, 0.1f) : 1.0f;
      const float height_from_width = frame.width * aspect_ratio;
      const float width_from_height = frame.height / aspect_ratio;
      const bool use_width = width_result.active || (!height_result.active &&
                                                     std::abs(height_from_width - frame.height) <=
                                                         std::abs(width_from_height - frame.width));
      if (use_width) {
        frame.width = width_result.active
                          ? (options.grid_enabled ? width_result.snapped_value : width_result.value)
                          : frame.width;
        frame.height = frame.width * aspect_ratio;
      } else {
        frame.height = height_result.active ? (options.grid_enabled ? height_result.snapped_value
                                                                    : height_result.value)
                                            : frame.height;
        frame.width = frame.height / aspect_ratio;
      }
      if (preserve_center) {
        frame.x = center_x - frame.width * 0.5f;
        frame.y = center_y - frame.height * 0.5f;
      } else {
        frame.x = origin_x;
        frame.y = origin_y;
      }
      if (width_result.active) {
        AppendUniqueGuidePosition(result.vertical_guides, frame.x + frame.width);
      }
      if (height_result.active) {
        AppendUniqueGuidePosition(result.horizontal_guides, frame.y + frame.height);
      }
    } else {
      frame.width = options.grid_enabled && width_result.active ? width_result.snapped_value
                                                                : width_result.value;
      frame.height = options.grid_enabled && height_result.active ? height_result.snapped_value
                                                                  : height_result.value;
      if (width_result.active) {
        AppendUniqueGuidePosition(result.vertical_guides, frame.x + frame.width);
      }
      if (height_result.active) {
        AppendUniqueGuidePosition(result.horizontal_guides, frame.y + frame.height);
      }
    }
  }

  result.frame = ResolveNormalizedControlFrame(
      NormalizedControlFrameFromResolvedFrame(frame, position_space, size_space, control.type),
      position_space, size_space, control.type);
  return result;
}

}  // namespace ui
}  // namespace xe
