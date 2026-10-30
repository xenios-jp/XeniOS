/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_APPLE_UI_NAVIGATION_H_
#define XENIA_UI_APPLE_UI_NAVIGATION_H_

#include <cstdint>
#include <unordered_map>

#include "xenia/hid/input.h"

namespace xe {
namespace ui {
namespace apple {

enum class NavigationDirection : uint8_t {
  kUp = 0,
  kDown = 1,
  kLeft = 2,
  kRight = 3,
};

struct ControllerActionSet {
  bool navigate_up = false;
  bool navigate_down = false;
  bool navigate_left = false;
  bool navigate_right = false;

  bool accept = false;        // A
  bool back = false;          // B
  bool context = false;       // X
  bool quick_action = false;  // Y
  bool guide = false;         // Guide

  bool section_prev = false;  // LB
  bool section_next = false;  // RB
  bool page_prev = false;     // LT
  bool page_next = false;     // RT

  bool Any() const;
  void SetDirection(NavigationDirection direction);
};

struct ControllerNavigationConfig {
  int16_t stick_deadzone = 12000;
  uint8_t trigger_threshold = 32;
  uint32_t repeat_initial_delay_ms = 300;
  uint32_t repeat_interval_ms = 75;
};

class ControllerNavigationMapper {
 public:
  explicit ControllerNavigationMapper(
      ControllerNavigationConfig config = ControllerNavigationConfig());

  // `now_ms` should be monotonic time in milliseconds.
  ControllerActionSet Update(const hid::X_INPUT_STATE& state, uint64_t now_ms);
  void Reset();

 private:
  enum class DirectionalSource : uint8_t {
    kNone = 0,
    kDpad = 1,
    kStick = 2,
  };

  struct ActiveDirection {
    bool active = false;
    NavigationDirection direction = NavigationDirection::kUp;
    DirectionalSource source = DirectionalSource::kNone;
  };

  static ActiveDirection ResolveDirection(const hid::X_INPUT_GAMEPAD& gamepad,
                                          int16_t stick_deadzone);

  ControllerActionSet BuildEdgeActions(
      const hid::X_INPUT_GAMEPAD& gamepad) const;
  void UpdateDirectionalRepeat(const ActiveDirection& direction,
                               uint64_t now_ms,
                               ControllerActionSet& out_actions);

  ControllerNavigationConfig config_;

  uint16_t prev_buttons_ = 0;
  bool prev_left_trigger_pressed_ = false;
  bool prev_right_trigger_pressed_ = false;

  bool repeat_active_ = false;
  NavigationDirection repeat_direction_ = NavigationDirection::kUp;
  DirectionalSource repeat_source_ = DirectionalSource::kNone;
  uint64_t next_repeat_ms_ = 0;
};

using FocusNodeId = uint32_t;
constexpr FocusNodeId kInvalidFocusNodeId = 0;

struct FocusNode {
  FocusNodeId id = kInvalidFocusNodeId;
  FocusNodeId up = kInvalidFocusNodeId;
  FocusNodeId down = kInvalidFocusNodeId;
  FocusNodeId left = kInvalidFocusNodeId;
  FocusNodeId right = kInvalidFocusNodeId;
  bool enabled = true;
};

class FocusGraph {
 public:
  void Clear();
  void AddOrUpdateNode(const FocusNode& node);
  bool HasNode(FocusNodeId id) const;

  void SetNodeEnabled(FocusNodeId id, bool enabled);

  FocusNodeId current() const { return current_; }
  bool SetCurrent(FocusNodeId id);

  // Returns the resulting focused node ID (or current if movement fails).
  FocusNodeId Move(NavigationDirection direction);

 private:
  FocusNodeId FirstEnabledNode() const;
  FocusNodeId ResolveEdge(const FocusNode& node,
                          NavigationDirection direction) const;

  std::unordered_map<FocusNodeId, FocusNode> nodes_;
  FocusNodeId current_ = kInvalidFocusNodeId;
};

}  // namespace apple
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_APPLE_UI_NAVIGATION_H_
