/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/shared/apple_ui_navigation.h"

#include <algorithm>
#include <cstdlib>

namespace xe {
namespace ui {
namespace apple {

bool ControllerActionSet::Any() const {
  return navigate_up || navigate_down || navigate_left || navigate_right ||
         accept || back || context || quick_action || guide || section_prev ||
         section_next || page_prev || page_next;
}

void ControllerActionSet::SetDirection(NavigationDirection direction) {
  switch (direction) {
    case NavigationDirection::kUp:
      navigate_up = true;
      break;
    case NavigationDirection::kDown:
      navigate_down = true;
      break;
    case NavigationDirection::kLeft:
      navigate_left = true;
      break;
    case NavigationDirection::kRight:
      navigate_right = true;
      break;
  }
}

ControllerNavigationMapper::ControllerNavigationMapper(
    ControllerNavigationConfig config)
    : config_(config) {}

ControllerActionSet ControllerNavigationMapper::Update(
    const hid::X_INPUT_STATE& state, uint64_t now_ms) {
  const hid::X_INPUT_GAMEPAD& gamepad = state.gamepad;
  ControllerActionSet actions = BuildEdgeActions(gamepad);

  const ActiveDirection direction =
      ResolveDirection(gamepad, config_.stick_deadzone);
  UpdateDirectionalRepeat(direction, now_ms, actions);

  prev_buttons_ = gamepad.buttons;
  prev_left_trigger_pressed_ =
      gamepad.left_trigger >= config_.trigger_threshold;
  prev_right_trigger_pressed_ =
      gamepad.right_trigger >= config_.trigger_threshold;

  return actions;
}

void ControllerNavigationMapper::Reset() {
  prev_buttons_ = 0;
  prev_left_trigger_pressed_ = false;
  prev_right_trigger_pressed_ = false;
  repeat_active_ = false;
  repeat_direction_ = NavigationDirection::kUp;
  repeat_source_ = DirectionalSource::kNone;
  next_repeat_ms_ = 0;
}

ControllerNavigationMapper::ActiveDirection
ControllerNavigationMapper::ResolveDirection(
    const hid::X_INPUT_GAMEPAD& gamepad, int16_t stick_deadzone) {
  const bool dpad_up = (gamepad.buttons & hid::X_INPUT_GAMEPAD_DPAD_UP) != 0;
  const bool dpad_down =
      (gamepad.buttons & hid::X_INPUT_GAMEPAD_DPAD_DOWN) != 0;
  const bool dpad_left =
      (gamepad.buttons & hid::X_INPUT_GAMEPAD_DPAD_LEFT) != 0;
  const bool dpad_right =
      (gamepad.buttons & hid::X_INPUT_GAMEPAD_DPAD_RIGHT) != 0;

  if (dpad_up) {
    return {true, NavigationDirection::kUp, DirectionalSource::kDpad};
  }
  if (dpad_down) {
    return {true, NavigationDirection::kDown, DirectionalSource::kDpad};
  }
  if (dpad_left) {
    return {true, NavigationDirection::kLeft, DirectionalSource::kDpad};
  }
  if (dpad_right) {
    return {true, NavigationDirection::kRight, DirectionalSource::kDpad};
  }

  const int16_t lx = gamepad.thumb_lx;
  const int16_t ly = gamepad.thumb_ly;
  const int32_t abs_lx = std::abs(static_cast<int32_t>(lx));
  const int32_t abs_ly = std::abs(static_cast<int32_t>(ly));

  if (abs_lx < stick_deadzone && abs_ly < stick_deadzone) {
    return {};
  }

  if (abs_ly >= abs_lx) {
    if (ly > 0) {
      return {true, NavigationDirection::kUp, DirectionalSource::kStick};
    }
    return {true, NavigationDirection::kDown, DirectionalSource::kStick};
  }
  if (lx < 0) {
    return {true, NavigationDirection::kLeft, DirectionalSource::kStick};
  }
  return {true, NavigationDirection::kRight, DirectionalSource::kStick};
}

ControllerActionSet ControllerNavigationMapper::BuildEdgeActions(
    const hid::X_INPUT_GAMEPAD& gamepad) const {
  ControllerActionSet actions;

  const uint16_t buttons = gamepad.buttons;
  const uint16_t pressed = buttons & ~prev_buttons_;

  actions.accept = (pressed & hid::X_INPUT_GAMEPAD_A) != 0;
  actions.back = (pressed & hid::X_INPUT_GAMEPAD_B) != 0;
  actions.context = (pressed & hid::X_INPUT_GAMEPAD_X) != 0;
  actions.quick_action = (pressed & hid::X_INPUT_GAMEPAD_Y) != 0;
  actions.guide = (pressed & hid::X_INPUT_GAMEPAD_GUIDE) != 0;
  actions.section_prev = (pressed & hid::X_INPUT_GAMEPAD_LEFT_SHOULDER) != 0;
  actions.section_next = (pressed & hid::X_INPUT_GAMEPAD_RIGHT_SHOULDER) != 0;

  const bool left_trigger_pressed =
      gamepad.left_trigger >= config_.trigger_threshold;
  const bool right_trigger_pressed =
      gamepad.right_trigger >= config_.trigger_threshold;
  actions.page_prev = left_trigger_pressed && !prev_left_trigger_pressed_;
  actions.page_next = right_trigger_pressed && !prev_right_trigger_pressed_;

  return actions;
}

void ControllerNavigationMapper::UpdateDirectionalRepeat(
    const ActiveDirection& direction, uint64_t now_ms,
    ControllerActionSet& out_actions) {
  if (!direction.active) {
    repeat_active_ = false;
    repeat_source_ = DirectionalSource::kNone;
    return;
  }

  const bool direction_changed = !repeat_active_ ||
                                 repeat_direction_ != direction.direction ||
                                 repeat_source_ != direction.source;

  if (direction_changed) {
    out_actions.SetDirection(direction.direction);
    repeat_active_ = true;
    repeat_direction_ = direction.direction;
    repeat_source_ = direction.source;
    next_repeat_ms_ = now_ms + config_.repeat_initial_delay_ms;
  } else if (now_ms >= next_repeat_ms_) {
    out_actions.SetDirection(direction.direction);
    const uint32_t interval = std::max(config_.repeat_interval_ms, 1u);
    do {
      next_repeat_ms_ += interval;
    } while (next_repeat_ms_ <= now_ms);
  }
}

void FocusGraph::Clear() {
  nodes_.clear();
  current_ = kInvalidFocusNodeId;
}

void FocusGraph::AddOrUpdateNode(const FocusNode& node) {
  if (node.id == kInvalidFocusNodeId) {
    return;
  }
  nodes_[node.id] = node;
  if (current_ == kInvalidFocusNodeId && node.enabled) {
    current_ = node.id;
  }
}

bool FocusGraph::HasNode(FocusNodeId id) const {
  return nodes_.find(id) != nodes_.end();
}

void FocusGraph::SetNodeEnabled(FocusNodeId id, bool enabled) {
  auto it = nodes_.find(id);
  if (it == nodes_.end()) {
    return;
  }
  it->second.enabled = enabled;
  if (id == current_ && !enabled) {
    current_ = FirstEnabledNode();
  }
}

bool FocusGraph::SetCurrent(FocusNodeId id) {
  auto it = nodes_.find(id);
  if (it == nodes_.end() || !it->second.enabled) {
    return false;
  }
  current_ = id;
  return true;
}

FocusNodeId FocusGraph::Move(NavigationDirection direction) {
  if (current_ == kInvalidFocusNodeId) {
    current_ = FirstEnabledNode();
    return current_;
  }

  auto it = nodes_.find(current_);
  if (it == nodes_.end() || !it->second.enabled) {
    current_ = FirstEnabledNode();
    return current_;
  }

  FocusNodeId candidate = ResolveEdge(it->second, direction);
  if (candidate == kInvalidFocusNodeId) {
    return current_;
  }
  auto next_it = nodes_.find(candidate);
  if (next_it == nodes_.end() || !next_it->second.enabled) {
    return current_;
  }
  current_ = candidate;
  return current_;
}

FocusNodeId FocusGraph::FirstEnabledNode() const {
  FocusNodeId first_enabled = kInvalidFocusNodeId;
  for (const auto& [id, node] : nodes_) {
    if (!node.enabled) {
      continue;
    }
    if (first_enabled == kInvalidFocusNodeId || id < first_enabled) {
      first_enabled = id;
    }
  }
  return first_enabled;
}

FocusNodeId FocusGraph::ResolveEdge(const FocusNode& node,
                                    NavigationDirection direction) const {
  switch (direction) {
    case NavigationDirection::kUp:
      return node.up;
    case NavigationDirection::kDown:
      return node.down;
    case NavigationDirection::kLeft:
      return node.left;
    case NavigationDirection::kRight:
      return node.right;
  }
  return kInvalidFocusNodeId;
}

}  // namespace apple
}  // namespace ui
}  // namespace xe
