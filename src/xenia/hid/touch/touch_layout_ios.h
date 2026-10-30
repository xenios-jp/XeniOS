/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_TOUCH_LAYOUT_IOS_H_
#define XENIA_HID_TOUCH_LAYOUT_IOS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/hid/input.h"

namespace xe {
namespace hid {
namespace touch {

enum class IOSTouchControlType : uint8_t {
  kMoveStick = 0,
  kLookSwipeZone,
  kActionButton,
  kPauseButton,
};

enum class IOSTouchControlShape : uint8_t {
  kCircle = 0,
  kRoundedRect,
};

enum class IOSTouchTintStyle : uint8_t {
  kAuto = 0,
  kAmber,
  kSky,
  kMint,
  kRose,
  kLime,
  kCoral,
  kSlate,
};

enum class IOSTouchAction : uint8_t {
  kNone = 0,
  kMove,
  kLook,
  kPauseMenu,
  kButtonA,
  kButtonB,
  kButtonX,
  kButtonY,
  kLeftBumper,
  kRightBumper,
  kLeftTrigger,
  kRightTrigger,
  kBack,
  kStart,
  kLeftThumb,
  kRightThumb,
  kDpadUp,
  kDpadDown,
  kDpadLeft,
  kDpadRight,
  kJump = kButtonA,
  kReloadInteract = kButtonX,
  kAim = kLeftTrigger,
  kFire = kRightTrigger,
};

enum class IOSTouchInteractionTrigger : uint8_t {
  kNone = 0,
  kHold,
  kHoldDrag,
  kDoubleTap,
  kDoubleTapForward,
};

enum class IOSTouchAnalogOutput : uint8_t {
  kNone = 0,
  kLook,
  kMove,
};

inline constexpr std::array<IOSTouchAction, 17> kIOSTouchEditableActions = {
    IOSTouchAction::kButtonA,     IOSTouchAction::kButtonB,
    IOSTouchAction::kButtonX,     IOSTouchAction::kButtonY,
    IOSTouchAction::kLeftBumper,  IOSTouchAction::kRightBumper,
    IOSTouchAction::kLeftTrigger, IOSTouchAction::kRightTrigger,
    IOSTouchAction::kBack,        IOSTouchAction::kStart,
    IOSTouchAction::kLeftThumb,   IOSTouchAction::kRightThumb,
    IOSTouchAction::kDpadUp,      IOSTouchAction::kDpadDown,
    IOSTouchAction::kDpadLeft,    IOSTouchAction::kDpadRight,
    IOSTouchAction::kNone,
};

inline constexpr std::array<IOSTouchAnalogOutput, 3>
    kIOSTouchEditableAnalogOutputs = {
        IOSTouchAnalogOutput::kNone,
        IOSTouchAnalogOutput::kLook,
        IOSTouchAnalogOutput::kMove,
};

inline constexpr std::array<IOSTouchTintStyle, 8> kIOSTouchEditableTintStyles =
    {
        IOSTouchTintStyle::kAuto,  IOSTouchTintStyle::kAmber,
        IOSTouchTintStyle::kSky,   IOSTouchTintStyle::kMint,
        IOSTouchTintStyle::kRose,  IOSTouchTintStyle::kLime,
        IOSTouchTintStyle::kCoral, IOSTouchTintStyle::kSlate,
};

inline constexpr std::size_t kMaxIOSTouchControls = 32;

struct IOSTouchPoint {
  float x = 0.0f;
  float y = 0.0f;
};

struct IOSTouchRect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

struct IOSTouchAnalogTuning {
  float deadzone = 0.0f;
  float activation_radius = 1.0f;
  float horizontal_scale = 1.0f;
  float vertical_scale = 1.0f;
  float diagonal_scale = 1.0f;
  float response_curve = 1.0f;
  float acceleration_scale = 0.0f;
  float smoothing = 0.0f;
  float max_output = 1.0f;
  bool invert_x = false;
  bool invert_y = false;
};

struct IOSTouchLayoutSpace {
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;

  bool IsEmpty() const { return width <= 0.0f || height <= 0.0f; }
};

struct IOSTouchInteractionBehavior {
  IOSTouchInteractionTrigger trigger = IOSTouchInteractionTrigger::kNone;
  IOSTouchAction action = IOSTouchAction::kNone;
  IOSTouchAnalogOutput analog_output = IOSTouchAnalogOutput::kNone;
  IOSTouchAnalogTuning analog_tuning;
  // Legacy alias for layouts saved before analog_output existed. New code
  // keeps this synchronized when analog_output == kLook so older readers still
  // understand hold-drag look behavior.
  bool enables_relative_look = false;
  float relative_look_scale = 1.0f;
  float hold_seconds = 0.30f;
  float drag_threshold_points = 14.0f;
};

struct IOSTouchControlDefinition {
  std::string identifier;
  std::string label;
  bool label_uses_default = true;
  bool label_hidden = false;
  IOSTouchControlType type = IOSTouchControlType::kActionButton;
  IOSTouchAction action = IOSTouchAction::kNone;
  IOSTouchControlShape shape = IOSTouchControlShape::kCircle;
  IOSTouchRect normalized_frame;
  // Optional portrait variant. When unset (`has_portrait_frame == false`),
  // `normalized_frame` is used in both orientations — preserving the legacy
  // single-frame behaviour for layouts authored before the per-orientation
  // split. When set, `portrait_normalized_frame` overrides `normalized_frame`
  // whenever the host view is rendered in portrait orientation, so a single
  // layout can hold separate landscape and portrait positions for the same
  // control without one orientation squashing the other.
  bool has_portrait_frame = false;
  IOSTouchRect portrait_normalized_frame;
  float deadzone = 0.0f;
  float activation_radius = 0.0f;
  float visual_opacity = 1.0f;
  IOSTouchTintStyle tint_style = IOSTouchTintStyle::kAuto;
  bool hold_while_captured = false;
  bool enables_relative_look = false;
  IOSTouchAnalogOutput drag_output = IOSTouchAnalogOutput::kNone;
  IOSTouchAnalogTuning analog_tuning;
  // Per-control look sensitivity multiplier, exposed in the touch editor.
  // For Look swipe zones: scales the swipe-points-per-full-deflection so
  // that 2.0x means full look output is reached with half as much swipe
  // (snappier aim) and 0.5x means twice as much swipe is needed.
  // For action buttons with `enables_relative_look`: scales the relative-look
  // output emitted while the button is held.
  // Effective editor-clamped range: 0.25 .. 4.0.
  float relative_look_scale = 1.0f;
  // While this control is held, scale all analog look / move output. Defaults
  // to 1.0 so existing layouts are unchanged; layouts can use this for
  // aim-down-sights style lower look sensitivity or flight controls that move
  // differently while LT/RB/etc. is held.
  float held_look_scale = 1.0f;
  float held_move_scale = 1.0f;
  // Move + D-Pad combo: when set on a kMoveStick, the control renders four
  // tappable D-Pad arrows around the stick base and the publish path emits
  // the corresponding D-Pad bit (Up/Down/Left/Right) while a finger is held
  // on an arrow. The centre area still drives the stick (analog left thumb,
  // or right thumb if `action == kLook`). Lets a single combo control replace
  // a separate Move stick + dedicated D-Pad cross in a layout — common for
  // Xbox 360 games that use both for movement and quick item swap.
  bool move_with_dpad_ring = false;
  IOSTouchInteractionBehavior secondary_behavior;
  uint8_t capture_priority = 0;
  uint16_t mapped_buttons = 0;
  uint8_t mapped_left_trigger = 0;
  uint8_t mapped_right_trigger = 0;
};

struct IOSTouchLayoutModel {
  std::string layout_id;
  std::string display_name;
  std::string author;
  std::string base_template;
  std::vector<IOSTouchControlDefinition> controls;
};

struct IOSTouchResolvedState {
  uint32_t packet_number = 0;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
  bool gameplay_enabled = false;
  bool pause_requested = false;
};

static_assert(std::is_trivially_copyable<IOSTouchResolvedState>::value,
              "IOSTouchResolvedState must remain trivially copyable");

class IOSTouchResolvedStateBuffer {
 public:
  IOSTouchResolvedStateBuffer() = default;

  void Store(const IOSTouchResolvedState& state);
  IOSTouchResolvedState Load() const;

 private:
  mutable std::mutex mutex_;
  IOSTouchResolvedState state_{};
};

class IOSTouchRuntimeModel {
 public:
  IOSTouchRuntimeModel();

  // Layout mutation/reads are expected on the UIKit main thread. Cross-thread
  // consumers should use resolved-state snapshots rather than touching layout.
  const IOSTouchLayoutModel& layout() const { return layout_; }
  IOSTouchLayoutModel& mutable_layout() { return layout_; }
  void SetLayout(IOSTouchLayoutModel layout);

  // Copies the current layout's controls into a snapshot that cross-thread
  // consumers (e.g. the HID driver answering guest capability queries while
  // the layout editor mutates the live layout) can read safely. Must be
  // called on the layout-owning (main) thread after structural layout
  // changes; SetLayout publishes automatically.
  void PublishControlsSnapshot();
  std::shared_ptr<const std::vector<IOSTouchControlDefinition>>
  LoadControlsSnapshot() const;

  void StoreResolvedState(const IOSTouchResolvedState& state);
  IOSTouchResolvedState LoadResolvedState() const;
  void ResetResolvedState();

 private:
  IOSTouchLayoutModel layout_;
  IOSTouchResolvedStateBuffer resolved_state_;
  mutable std::mutex controls_snapshot_mutex_;
  std::shared_ptr<const std::vector<IOSTouchControlDefinition>>
      controls_snapshot_;
};

IOSTouchLayoutModel CreateDefaultIOSFPSLayoutModel();
IOSTouchControlDefinition CreateDefaultIOSTouchControlDefinition(
    IOSTouchControlType type);
toml::table EncodeIOSTouchLayoutModel(const IOSTouchLayoutModel& layout);
bool ApplyIOSTouchLayoutModel(const toml::table& table,
                              IOSTouchLayoutModel* layout);
bool IsEditableIOSTouchAction(IOSTouchAction action);
bool IsSupportedIOSTouchPrimaryAction(IOSTouchControlType control_type,
                                      IOSTouchAction action);
const char* IOSTouchActionDisplayName(IOSTouchAction action);
const char* IOSTouchInteractionTriggerDisplayName(
    IOSTouchInteractionTrigger trigger);
const char* IOSTouchAnalogOutputDisplayName(IOSTouchAnalogOutput output);
float DefaultIOSTouchHoldSecondsForInteractionTrigger(
    IOSTouchInteractionTrigger trigger);
bool IOSTouchControlHasCustomLabel(const IOSTouchControlDefinition& control);
std::string IOSTouchConfiguredControlLabel(
    const IOSTouchControlDefinition& control);
std::string IOSTouchVisibleControlLabel(
    const IOSTouchControlDefinition& control);
void SetIOSTouchControlCustomLabel(std::string label,
                                   IOSTouchControlDefinition* control);
void ResetIOSTouchControlLabel(IOSTouchControlDefinition* control);
bool ConfigureIOSTouchControlAction(IOSTouchAction action,
                                    IOSTouchControlDefinition* control);
bool IsValidIOSTouchLayoutModel(const IOSTouchLayoutModel& layout);
IOSTouchAction NextEditableIOSTouchAction(IOSTouchAction action);
const char* IOSTouchTintStyleDisplayName(IOSTouchTintStyle tint_style);
IOSTouchTintStyle NextIOSTouchTintStyle(IOSTouchTintStyle tint_style);
IOSTouchRect ResolveIOSTouchRect(const IOSTouchRect& normalized_rect,
                                 const IOSTouchLayoutSpace& safe_area);
bool IOSTouchRectContainsPoint(const IOSTouchRect& rect,
                               const IOSTouchPoint& point);

// Returns the normalized frame the engine should use for the given
// orientation. In portrait, falls through to the landscape `normalized_frame`
// whenever the control has not yet authored a portrait override
// (`has_portrait_frame == false`). Used by every read site (layout
// resolution, snap target gathering, hit-test fallback, capture seeding) so
// orientation switching becomes a single choke point.
const IOSTouchRect& ActiveControlFrameForOrientation(
    const IOSTouchControlDefinition& control, bool is_portrait);

// Mutable counterpart used by editor mutation paths (drag commit, pinch
// resize, mirror, size adjust, duplicate, etc.). When the device is in
// portrait orientation and the control has no portrait frame yet
// (`has_portrait_frame == false`), this lazily promotes the existing
// landscape `normalized_frame` into `portrait_normalized_frame` and sets
// `has_portrait_frame = true`. The first portrait edit therefore starts
// from the landscape baseline rather than (0, 0), which is what makes the
// rotate-then-nudge editor flow feel right.
IOSTouchRect& MutableActiveControlFrameForOrientation(
    IOSTouchControlDefinition& control, bool is_portrait);

}  // namespace touch
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_TOUCH_LAYOUT_IOS_H_
