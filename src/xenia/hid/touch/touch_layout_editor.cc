/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/touch/touch_layout_editor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace xe {
namespace hid {
namespace touch {
namespace {

float MaxNormalizedEditorControlSize(IOSTouchControlType control_type) {
  return control_type == IOSTouchControlType::kLookSwipeZone ? 1.0f : 0.98f;
}

bool ControlParticipatesInPlacementAvoidance(
    const IOSTouchControlDefinition& control) {
  return control.type != IOSTouchControlType::kLookSwipeZone;
}

float TouchRectArea(const IOSTouchRect& rect) {
  return std::max(rect.width, 0.0f) * std::max(rect.height, 0.0f);
}

float TouchRectIntersectionArea(const IOSTouchRect& a, const IOSTouchRect& b) {
  const float min_x = std::max(a.x, b.x);
  const float min_y = std::max(a.y, b.y);
  const float max_x = std::min(a.x + a.width, b.x + b.width);
  const float max_y = std::min(a.y + a.height, b.y + b.height);
  return std::max(max_x - min_x, 0.0f) * std::max(max_y - min_y, 0.0f);
}

float PlacementOverlapRatioSum(const IOSTouchLayoutModel& layout,
                               const IOSTouchRect& candidate,
                               bool is_portrait) {
  const float candidate_area = TouchRectArea(candidate);
  if (candidate_area <= 0.0f) {
    return std::numeric_limits<float>::max();
  }

  float overlap_ratio_sum = 0.0f;
  for (const auto& control : layout.controls) {
    if (!ControlParticipatesInPlacementAvoidance(control)) {
      continue;
    }
    const IOSTouchRect& active_frame =
        ActiveControlFrameForOrientation(control, is_portrait);
    overlap_ratio_sum +=
        TouchRectIntersectionArea(candidate, active_frame) / candidate_area;
  }
  return overlap_ratio_sum;
}

void AppendCandidateFrame(std::vector<IOSTouchRect>* candidates,
                          const IOSTouchRect& frame,
                          IOSTouchControlType control_type) {
  if (!candidates) {
    return;
  }
  IOSTouchRect clamped = ClampIOSTouchEditorControlFrame(frame, control_type);
  const bool exists =
      std::any_of(candidates->begin(), candidates->end(),
                  [&clamped](const IOSTouchRect& existing) {
                    return std::abs(existing.x - clamped.x) < 0.001f &&
                           std::abs(existing.y - clamped.y) < 0.001f &&
                           std::abs(existing.width - clamped.width) < 0.001f &&
                           std::abs(existing.height - clamped.height) < 0.001f;
                  });
  if (!exists) {
    candidates->push_back(clamped);
  }
}

}  // namespace

bool IOSTouchLayoutHasActionBinding(const IOSTouchLayoutModel& layout,
                                    IOSTouchAction action) {
  return std::any_of(layout.controls.begin(), layout.controls.end(),
                     [action](const IOSTouchControlDefinition& control) {
                       return control.type ==
                                  IOSTouchControlType::kActionButton &&
                              control.action == action;
                     });
}

std::string MakeUniqueIOSTouchActionButtonIdentifier(
    const IOSTouchLayoutModel& layout) {
  int suffix = 1;
  while (true) {
    std::string identifier = suffix == 1
                                 ? "action_button"
                                 : ("action_button_" + std::to_string(suffix));
    const bool exists =
        std::any_of(layout.controls.begin(), layout.controls.end(),
                    [&identifier](const IOSTouchControlDefinition& control) {
                      return control.identifier == identifier;
                    });
    if (!exists) {
      return identifier;
    }
    ++suffix;
  }
}

IOSTouchAction SuggestedNewIOSTouchActionButtonBinding(
    const IOSTouchLayoutModel& layout) {
  const IOSTouchAction kSuggestedBindings[] = {
      IOSTouchAction::kButtonB,     IOSTouchAction::kButtonY,
      IOSTouchAction::kLeftBumper,  IOSTouchAction::kRightBumper,
      IOSTouchAction::kButtonA,     IOSTouchAction::kButtonX,
      IOSTouchAction::kLeftTrigger, IOSTouchAction::kRightTrigger,
      IOSTouchAction::kBack,        IOSTouchAction::kStart,
      IOSTouchAction::kLeftThumb,   IOSTouchAction::kRightThumb,
      IOSTouchAction::kDpadUp,      IOSTouchAction::kDpadDown,
      IOSTouchAction::kDpadLeft,    IOSTouchAction::kDpadRight,
  };
  for (IOSTouchAction binding : kSuggestedBindings) {
    if (!IOSTouchLayoutHasActionBinding(layout, binding)) {
      return binding;
    }
  }
  return IOSTouchAction::kButtonA;
}

IOSTouchRect SuggestedNewIOSTouchActionButtonFrame(IOSTouchAction action) {
  switch (action) {
    case IOSTouchAction::kButtonB:
      return IOSTouchRect{0.88f, 0.66f, 0.11f, 0.13f};
    case IOSTouchAction::kButtonY:
      return IOSTouchRect{0.84f, 0.36f, 0.11f, 0.13f};
    case IOSTouchAction::kLeftBumper:
      return IOSTouchRect{0.58f, 0.49f, 0.11f, 0.13f};
    case IOSTouchAction::kRightBumper:
      return IOSTouchRect{0.72f, 0.31f, 0.11f, 0.13f};
    case IOSTouchAction::kBack:
      return IOSTouchRect{0.44f, 0.03f, 0.10f, 0.10f};
    case IOSTouchAction::kStart:
      return IOSTouchRect{0.56f, 0.03f, 0.10f, 0.10f};
    case IOSTouchAction::kLeftThumb:
      return IOSTouchRect{0.08f, 0.79f, 0.11f, 0.13f};
    case IOSTouchAction::kRightThumb:
      return IOSTouchRect{0.84f, 0.84f, 0.11f, 0.13f};
    case IOSTouchAction::kDpadUp:
      return IOSTouchRect{0.24f, 0.22f, 0.10f, 0.12f};
    case IOSTouchAction::kDpadDown:
      return IOSTouchRect{0.24f, 0.36f, 0.10f, 0.12f};
    case IOSTouchAction::kDpadLeft:
      return IOSTouchRect{0.17f, 0.29f, 0.10f, 0.12f};
    case IOSTouchAction::kDpadRight:
      return IOSTouchRect{0.31f, 0.29f, 0.10f, 0.12f};
    case IOSTouchAction::kButtonA:
      return IOSTouchRect{0.86f, 0.54f, 0.13f, 0.14f};
    case IOSTouchAction::kButtonX:
      return IOSTouchRect{0.72f, 0.47f, 0.13f, 0.14f};
    case IOSTouchAction::kLeftTrigger:
      return IOSTouchRect{0.60f, 0.67f, 0.13f, 0.14f};
    case IOSTouchAction::kRightTrigger:
      return IOSTouchRect{0.75f, 0.74f, 0.23f, 0.17f};
    case IOSTouchAction::kNone:
    case IOSTouchAction::kMove:
    case IOSTouchAction::kLook:
    case IOSTouchAction::kPauseMenu:
    default:
      return IOSTouchRect{0.80f, 0.40f, 0.12f, 0.14f};
  }
}

IOSTouchRect ClampIOSTouchEditorControlFrame(const IOSTouchRect& rect,
                                             IOSTouchControlType control_type) {
  IOSTouchRect result = rect;
  const float max_control_size = MaxNormalizedEditorControlSize(control_type);
  result.width = std::clamp(result.width, 0.05f, max_control_size);
  result.height = std::clamp(result.height, 0.05f, max_control_size);
  result.x = std::clamp(result.x, 0.0f, 1.0f - result.width);
  result.y = std::clamp(result.y, 0.0f, 1.0f - result.height);
  return result;
}

IOSTouchRect FindAvailableIOSTouchEditorControlFrame(
    const IOSTouchLayoutModel& layout, const IOSTouchRect& preferred_frame,
    IOSTouchControlType control_type, bool is_portrait) {
  IOSTouchRect preferred =
      ClampIOSTouchEditorControlFrame(preferred_frame, control_type);
  if (control_type == IOSTouchControlType::kLookSwipeZone) {
    return preferred;
  }

  constexpr float kPlacementGap = 0.018f;
  std::vector<IOSTouchRect> candidates;
  candidates.reserve(160);
  AppendCandidateFrame(&candidates, preferred, control_type);

  const float step_x = std::max(preferred.width + kPlacementGap, 0.075f);
  const float step_y = std::max(preferred.height + kPlacementGap, 0.075f);
  for (int radius = 1; radius <= 8; ++radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        if (std::max(std::abs(dx), std::abs(dy)) != radius) {
          continue;
        }
        AppendCandidateFrame(
            &candidates,
            IOSTouchRect{preferred.x + dx * step_x, preferred.y + dy * step_y,
                         preferred.width, preferred.height},
            control_type);
      }
    }
  }

  auto append_grid = [&](float min_x, float max_x, float min_y, float max_y) {
    for (float y = min_y; y <= max_y - preferred.height + 0.001f; y += step_y) {
      for (float x = min_x; x <= max_x - preferred.width + 0.001f;
           x += step_x) {
        AppendCandidateFrame(
            &candidates, IOSTouchRect{x, y, preferred.width, preferred.height},
            control_type);
      }
    }
  };
  append_grid(0.52f, 0.98f, 0.18f, 0.92f);
  append_grid(0.06f, 0.98f, 0.06f, 0.92f);

  const float preferred_center_x = preferred.x + preferred.width * 0.5f;
  const float preferred_center_y = preferred.y + preferred.height * 0.5f;
  IOSTouchRect best = preferred;
  float best_score = std::numeric_limits<float>::max();
  for (const IOSTouchRect& candidate : candidates) {
    const float candidate_center_x = candidate.x + candidate.width * 0.5f;
    const float candidate_center_y = candidate.y + candidate.height * 0.5f;
    const float distance = std::hypot(candidate_center_x - preferred_center_x,
                                      candidate_center_y - preferred_center_y);
    const float overlap_ratio =
        PlacementOverlapRatioSum(layout, candidate, is_portrait);
    const float left_side_penalty = candidate.x < 0.45f ? 0.04f : 0.0f;
    const float score = overlap_ratio * 1000.0f + distance + left_side_penalty;
    if (score < best_score) {
      best_score = score;
      best = candidate;
    }
  }
  return best;
}

bool AddSuggestedActionButtonToIOSTouchLayout(
    IOSTouchLayoutModel* layout, bool is_portrait,
    std::string* selected_identifier_out) {
  if (!layout || layout->controls.size() >= kMaxIOSTouchControls) {
    return false;
  }

  const IOSTouchAction action =
      SuggestedNewIOSTouchActionButtonBinding(*layout);
  IOSTouchControlDefinition control;
  control.identifier = MakeUniqueIOSTouchActionButtonIdentifier(*layout);
  control.type = IOSTouchControlType::kActionButton;
  control.shape = IOSTouchControlShape::kCircle;
  control.normalized_frame = FindAvailableIOSTouchEditorControlFrame(
      *layout, SuggestedNewIOSTouchActionButtonFrame(action), control.type,
      is_portrait);
  if (is_portrait) {
    control.has_portrait_frame = true;
    control.portrait_normalized_frame = control.normalized_frame;
  }
  control.activation_radius = 0.5f;
  control.visual_opacity = 0.92f;
  control.capture_priority = 232;
  ConfigureIOSTouchControlAction(action, &control);
  const std::string selected_identifier = control.identifier;
  layout->controls.push_back(std::move(control));
  if (selected_identifier_out) {
    *selected_identifier_out = selected_identifier;
  }
  return true;
}

bool MirrorIOSTouchLayoutControlHorizontally(IOSTouchLayoutModel* layout,
                                             std::size_t control_index,
                                             bool is_portrait) {
  if (!layout || control_index >= layout->controls.size()) {
    return false;
  }

  IOSTouchControlDefinition& control = layout->controls[control_index];
  IOSTouchRect& active_frame =
      MutableActiveControlFrameForOrientation(control, is_portrait);
  active_frame.x =
      std::clamp(1.0f - active_frame.x - active_frame.width, 0.0f, 1.0f);
  return true;
}

bool CopyIOSTouchLayoutFramesAcrossOrientations(IOSTouchLayoutModel* layout,
                                                bool from_landscape) {
  if (!layout || layout->controls.empty()) {
    return false;
  }

  bool any_changed = false;
  for (auto& control : layout->controls) {
    if (from_landscape) {
      control.portrait_normalized_frame = control.normalized_frame;
      control.has_portrait_frame = true;
      any_changed = true;
    } else if (control.has_portrait_frame) {
      control.normalized_frame = control.portrait_normalized_frame;
      any_changed = true;
    }
  }
  return any_changed;
}

bool DuplicateIOSTouchLayoutActionButton(IOSTouchLayoutModel* layout,
                                         std::size_t source_control_index,
                                         bool is_portrait,
                                         std::string* selected_identifier_out) {
  if (!layout || source_control_index >= layout->controls.size() ||
      layout->controls.size() >= kMaxIOSTouchControls) {
    return false;
  }

  const auto& source_control = layout->controls[source_control_index];
  if (source_control.type != IOSTouchControlType::kActionButton) {
    return false;
  }

  IOSTouchControlDefinition duplicate = source_control;
  duplicate.identifier = MakeUniqueIOSTouchActionButtonIdentifier(*layout);
  IOSTouchRect& active_frame =
      MutableActiveControlFrameForOrientation(duplicate, is_portrait);
  active_frame = FindAvailableIOSTouchEditorControlFrame(
      *layout,
      IOSTouchRect{active_frame.x + 0.03f, active_frame.y + 0.03f,
                   active_frame.width, active_frame.height},
      duplicate.type, is_portrait);
  const std::string selected_identifier = duplicate.identifier;
  layout->controls.push_back(std::move(duplicate));
  if (selected_identifier_out) {
    *selected_identifier_out = selected_identifier;
  }
  return true;
}

bool DeleteIOSTouchLayoutControl(IOSTouchLayoutModel* layout,
                                 std::size_t control_index) {
  if (!layout || control_index >= layout->controls.size() ||
      layout->controls.size() <= 1) {
    return false;
  }
  layout->controls.erase(layout->controls.begin() + control_index);
  return true;
}

}  // namespace touch
}  // namespace hid
}  // namespace xe
