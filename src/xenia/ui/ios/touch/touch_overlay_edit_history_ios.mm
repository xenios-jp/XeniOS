/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/touch/touch_overlay_edit_history_ios.h"

#include <cmath>
#include <utility>

namespace xe::ui::ios::touch_overlay {
namespace {

constexpr size_t kTouchEditHistoryLimit = 64;

bool TouchFloatNear(float left, float right) { return std::abs(left - right) < 0.0005f; }

bool TouchRectsEqual(const xe::hid::touch::IOSTouchRect& left,
                     const xe::hid::touch::IOSTouchRect& right) {
  return TouchFloatNear(left.x, right.x) && TouchFloatNear(left.y, right.y) &&
         TouchFloatNear(left.width, right.width) && TouchFloatNear(left.height, right.height);
}

bool TouchInteractionBehaviorsEqual(const xe::hid::touch::IOSTouchInteractionBehavior& left,
                                    const xe::hid::touch::IOSTouchInteractionBehavior& right) {
  return left.trigger == right.trigger && left.action == right.action &&
         left.analog_output == right.analog_output &&
         TouchFloatNear(left.analog_tuning.deadzone, right.analog_tuning.deadzone) &&
         TouchFloatNear(left.analog_tuning.activation_radius,
                        right.analog_tuning.activation_radius) &&
         TouchFloatNear(left.analog_tuning.horizontal_scale,
                        right.analog_tuning.horizontal_scale) &&
         TouchFloatNear(left.analog_tuning.vertical_scale, right.analog_tuning.vertical_scale) &&
         TouchFloatNear(left.analog_tuning.diagonal_scale, right.analog_tuning.diagonal_scale) &&
         TouchFloatNear(left.analog_tuning.response_curve, right.analog_tuning.response_curve) &&
         TouchFloatNear(left.analog_tuning.acceleration_scale,
                        right.analog_tuning.acceleration_scale) &&
         TouchFloatNear(left.analog_tuning.smoothing, right.analog_tuning.smoothing) &&
         TouchFloatNear(left.analog_tuning.max_output, right.analog_tuning.max_output) &&
         left.analog_tuning.invert_x == right.analog_tuning.invert_x &&
         left.analog_tuning.invert_y == right.analog_tuning.invert_y &&
         left.enables_relative_look == right.enables_relative_look &&
         TouchFloatNear(left.relative_look_scale, right.relative_look_scale) &&
         TouchFloatNear(left.hold_seconds, right.hold_seconds) &&
         TouchFloatNear(left.drag_threshold_points, right.drag_threshold_points);
}

bool TouchControlDefinitionsEqual(const xe::hid::touch::IOSTouchControlDefinition& left,
                                  const xe::hid::touch::IOSTouchControlDefinition& right) {
  return left.identifier == right.identifier && left.label == right.label &&
         left.label_uses_default == right.label_uses_default &&
         left.label_hidden == right.label_hidden && left.type == right.type &&
         left.action == right.action && left.shape == right.shape &&
         TouchRectsEqual(left.normalized_frame, right.normalized_frame) &&
         left.has_portrait_frame == right.has_portrait_frame &&
         (!left.has_portrait_frame ||
          TouchRectsEqual(left.portrait_normalized_frame, right.portrait_normalized_frame)) &&
         TouchFloatNear(left.deadzone, right.deadzone) &&
         TouchFloatNear(left.activation_radius, right.activation_radius) &&
         TouchFloatNear(left.visual_opacity, right.visual_opacity) &&
         left.tint_style == right.tint_style &&
         left.hold_while_captured == right.hold_while_captured &&
         left.enables_relative_look == right.enables_relative_look &&
         left.drag_output == right.drag_output &&
         TouchFloatNear(left.analog_tuning.deadzone, right.analog_tuning.deadzone) &&
         TouchFloatNear(left.analog_tuning.activation_radius,
                        right.analog_tuning.activation_radius) &&
         TouchFloatNear(left.analog_tuning.horizontal_scale,
                        right.analog_tuning.horizontal_scale) &&
         TouchFloatNear(left.analog_tuning.vertical_scale, right.analog_tuning.vertical_scale) &&
         TouchFloatNear(left.analog_tuning.diagonal_scale, right.analog_tuning.diagonal_scale) &&
         TouchFloatNear(left.analog_tuning.response_curve, right.analog_tuning.response_curve) &&
         TouchFloatNear(left.analog_tuning.acceleration_scale,
                        right.analog_tuning.acceleration_scale) &&
         TouchFloatNear(left.analog_tuning.smoothing, right.analog_tuning.smoothing) &&
         TouchFloatNear(left.analog_tuning.max_output, right.analog_tuning.max_output) &&
         left.analog_tuning.invert_x == right.analog_tuning.invert_x &&
         left.analog_tuning.invert_y == right.analog_tuning.invert_y &&
         TouchFloatNear(left.relative_look_scale, right.relative_look_scale) &&
         TouchFloatNear(left.held_look_scale, right.held_look_scale) &&
         TouchFloatNear(left.held_move_scale, right.held_move_scale) &&
         TouchInteractionBehaviorsEqual(left.secondary_behavior, right.secondary_behavior) &&
         left.capture_priority == right.capture_priority &&
         left.mapped_buttons == right.mapped_buttons &&
         left.mapped_left_trigger == right.mapped_left_trigger &&
         left.mapped_right_trigger == right.mapped_right_trigger;
}

bool TouchLayoutModelsEqual(const xe::hid::touch::IOSTouchLayoutModel& left,
                            const xe::hid::touch::IOSTouchLayoutModel& right) {
  if (left.layout_id != right.layout_id || left.display_name != right.display_name ||
      left.author != right.author || left.base_template != right.base_template ||
      left.controls.size() != right.controls.size()) {
    return false;
  }
  for (size_t index = 0; index < left.controls.size(); ++index) {
    if (!TouchControlDefinitionsEqual(left.controls[index], right.controls[index])) {
      return false;
    }
  }
  return true;
}

}  // namespace

void TouchOverlayEditHistoryIOS::Reset() {
  change_active_ = false;
  pending_change_ = Entry{};
  undo_history_.clear();
  redo_history_.clear();
}

void TouchOverlayEditHistoryIOS::SeedIfNeeded(const xe::hid::touch::IOSTouchLayoutModel& layout,
                                              std::string selected_control_identifier) {
  if (!undo_history_.empty()) {
    return;
  }
  undo_history_.push_back(Entry{layout, std::move(selected_control_identifier)});
}

void TouchOverlayEditHistoryIOS::BeginChange(const xe::hid::touch::IOSTouchLayoutModel& layout,
                                             std::string selected_control_identifier) {
  if (change_active_) {
    return;
  }
  pending_change_ = Entry{layout, std::move(selected_control_identifier)};
  change_active_ = true;
}

bool TouchOverlayEditHistoryIOS::FinishChange(const xe::hid::touch::IOSTouchLayoutModel& layout,
                                              std::string selected_control_identifier) {
  if (!change_active_) {
    return false;
  }
  change_active_ = false;

  Entry previous = std::move(pending_change_);
  pending_change_ = Entry{};
  Entry current{layout, std::move(selected_control_identifier)};
  if (TouchLayoutModelsEqual(previous.layout, current.layout)) {
    return false;
  }

  if (undo_history_.empty() || !EntriesEqual(undo_history_.back(), previous)) {
    PushUndoEntry(std::move(previous));
  }
  if (!undo_history_.empty() && EntriesEqual(undo_history_.back(), current)) {
    redo_history_.clear();
    return true;
  }

  PushUndoEntry(std::move(current));
  redo_history_.clear();
  Trim();
  return true;
}

void TouchOverlayEditHistoryIOS::CancelChange() {
  change_active_ = false;
  pending_change_ = Entry{};
}

bool TouchOverlayEditHistoryIOS::CanUndo() const { return undo_history_.size() > 1; }

bool TouchOverlayEditHistoryIOS::CanRedo() const { return !redo_history_.empty(); }

bool TouchOverlayEditHistoryIOS::Undo(xe::hid::touch::IOSTouchLayoutModel* layout,
                                      std::string* selected_control_identifier) {
  if (!CanUndo() || !layout || !selected_control_identifier) {
    return false;
  }
  redo_history_.push_back(std::move(undo_history_.back()));
  undo_history_.pop_back();
  const Entry& target = undo_history_.back();
  *layout = target.layout;
  *selected_control_identifier = target.selected_control_identifier;
  Trim();
  return true;
}

bool TouchOverlayEditHistoryIOS::Redo(xe::hid::touch::IOSTouchLayoutModel* layout,
                                      std::string* selected_control_identifier) {
  if (!CanRedo() || !layout || !selected_control_identifier) {
    return false;
  }
  Entry target = std::move(redo_history_.back());
  redo_history_.pop_back();
  *layout = target.layout;
  *selected_control_identifier = target.selected_control_identifier;
  undo_history_.push_back(std::move(target));
  Trim();
  return true;
}

bool TouchOverlayEditHistoryIOS::EntriesEqual(const Entry& left, const Entry& right) {
  return left.selected_control_identifier == right.selected_control_identifier &&
         TouchLayoutModelsEqual(left.layout, right.layout);
}

void TouchOverlayEditHistoryIOS::Trim() {
  if (undo_history_.size() > kTouchEditHistoryLimit) {
    const size_t remove_count = undo_history_.size() - kTouchEditHistoryLimit;
    undo_history_.erase(undo_history_.begin(), undo_history_.begin() + remove_count);
  }
  if (redo_history_.size() > kTouchEditHistoryLimit) {
    const size_t remove_count = redo_history_.size() - kTouchEditHistoryLimit;
    redo_history_.erase(redo_history_.begin(), redo_history_.begin() + remove_count);
  }
}

void TouchOverlayEditHistoryIOS::PushUndoEntry(Entry entry) {
  undo_history_.push_back(std::move(entry));
}

}  // namespace xe::ui::ios::touch_overlay
