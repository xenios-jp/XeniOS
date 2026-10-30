/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/touch/touch_layout_library_ios.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <sstream>
#include <utility>

#include "xenia/config.h"
#include "xenia/ui/ios/touch/touch_overlay_style_ios.h"

namespace xe {
namespace ui {
namespace {

struct IOSTouchOfficialLayoutPreset {
  const char* local_id = nullptr;
  const char* display_name = nullptr;
  xe::hid::touch::IOSTouchLayoutModel (*factory)() = nullptr;
};

void SetPortraitFrame(xe::hid::touch::IOSTouchRect frame,
                      xe::hid::touch::IOSTouchControlDefinition* control) {
  control->has_portrait_frame = true;
  control->portrait_normalized_frame = frame;
}

void ClearHeldSlide(xe::hid::touch::IOSTouchControlDefinition* control) {
  if (!control) {
    return;
  }
  control->enables_relative_look = false;
  control->drag_output = xe::hid::touch::IOSTouchAnalogOutput::kNone;
  control->relative_look_scale = 1.0f;
  control->analog_tuning.horizontal_scale = 1.0f;
  control->analog_tuning.vertical_scale = 1.0f;
}

xe::hid::touch::IOSTouchInteractionBehavior MakeSecondaryAction(
    xe::hid::touch::IOSTouchInteractionTrigger trigger,
    xe::hid::touch::IOSTouchAction action) {
  xe::hid::touch::IOSTouchInteractionBehavior behavior;
  behavior.trigger = trigger;
  behavior.action = action;
  behavior.hold_seconds =
      xe::hid::touch::DefaultIOSTouchHoldSecondsForInteractionTrigger(trigger);
  return behavior;
}

xe::hid::touch::IOSTouchLayoutModel MakeOfficialTouchLayout(
    const char* local_id, const char* display_name) {
  xe::hid::touch::IOSTouchLayoutModel layout;
  layout.layout_id = local_id;
  layout.display_name = display_name;
  layout.author = "XeniOS";
  layout.base_template = local_id;
  layout.controls.reserve(20);
  return layout;
}

void AddOfficialTouchControl(
    xe::hid::touch::IOSTouchLayoutModel* layout,
    xe::hid::touch::IOSTouchControlDefinition control,
    const xe::hid::touch::IOSTouchRect& portrait_frame) {
  if (!layout) {
    return;
  }
  SetPortraitFrame(portrait_frame, &control);
  layout->controls.push_back(std::move(control));
}

xe::hid::touch::IOSTouchControlDefinition MakeOfficialTouchMoveStick(
    const xe::hid::touch::IOSTouchRect& frame,
    bool move_with_dpad_ring = false, uint8_t capture_priority = 220,
    const char* label = nullptr) {
  xe::hid::touch::IOSTouchControlDefinition control =
      xe::hid::touch::CreateDefaultIOSTouchControlDefinition(
          xe::hid::touch::IOSTouchControlType::kMoveStick);
  control.normalized_frame = frame;
  control.activation_radius = 0.46f;
  control.analog_tuning.activation_radius = control.activation_radius;
  control.visual_opacity = 0.78f;
  control.move_with_dpad_ring = move_with_dpad_ring;
  control.secondary_behavior = xe::hid::touch::IOSTouchInteractionBehavior{};
  control.capture_priority = capture_priority;
  if (label) {
    xe::hid::touch::SetIOSTouchControlCustomLabel(label, &control);
  }
  return control;
}

xe::hid::touch::IOSTouchControlDefinition MakeOfficialTouchLookBackdrop(
    uint8_t capture_priority = 8) {
  xe::hid::touch::IOSTouchControlDefinition control =
      xe::hid::touch::CreateDefaultIOSTouchControlDefinition(
          xe::hid::touch::IOSTouchControlType::kLookSwipeZone);
  control.identifier = "look_background";
  control.normalized_frame = xe::hid::touch::IOSTouchRect{0.0f, 0.0f, 1.0f,
                                                          1.0f};
  control.visual_opacity = 0.0f;
  control.capture_priority = capture_priority;
  control.drag_output = xe::hid::touch::IOSTouchAnalogOutput::kLook;
  xe::hid::touch::SetIOSTouchControlCustomLabel("Look Background", &control);
  return control;
}

xe::hid::touch::IOSTouchControlDefinition MakeOfficialTouchPauseButton(
    const xe::hid::touch::IOSTouchRect& frame, uint8_t capture_priority = 255) {
  xe::hid::touch::IOSTouchControlDefinition control =
      xe::hid::touch::CreateDefaultIOSTouchControlDefinition(
          xe::hid::touch::IOSTouchControlType::kPauseButton);
  control.normalized_frame = frame;
  control.visual_opacity = 0.92f;
  control.capture_priority = capture_priority;
  return control;
}

xe::hid::touch::IOSTouchControlDefinition MakeOfficialTouchActionButton(
    const char* identifier, xe::hid::touch::IOSTouchAction action,
    const xe::hid::touch::IOSTouchRect& frame, uint8_t capture_priority,
    xe::hid::touch::IOSTouchTintStyle tint_style =
        xe::hid::touch::IOSTouchTintStyle::kAuto,
    xe::hid::touch::IOSTouchControlShape shape =
        xe::hid::touch::IOSTouchControlShape::kCircle,
    bool keep_held_slide_look = false, const char* label = nullptr) {
  xe::hid::touch::IOSTouchControlDefinition control;
  control.identifier = identifier;
  control.type = xe::hid::touch::IOSTouchControlType::kActionButton;
  control.shape = shape;
  control.normalized_frame = frame;
  control.activation_radius = 0.5f;
  control.analog_tuning.activation_radius = control.activation_radius;
  control.visual_opacity = 0.92f;
  control.capture_priority = capture_priority;
  control.tint_style = tint_style;
  xe::hid::touch::ConfigureIOSTouchControlAction(action, &control);
  if (!keep_held_slide_look) {
    ClearHeldSlide(&control);
  }
  if (label) {
    xe::hid::touch::SetIOSTouchControlCustomLabel(label, &control);
  }
  return control;
}

void AddTopSystemButtons(xe::hid::touch::IOSTouchLayoutModel* layout) {
  using namespace xe::hid::touch;
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchPauseButton(IOSTouchRect{0.040f, 0.045f, 0.120f,
                                                0.112f}),
      IOSTouchRect{0.055f, 0.025f, 0.180f, 0.058f});
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchActionButton("back_button", IOSTouchAction::kBack,
                                    IOSTouchRect{0.390f, 0.045f, 0.080f,
                                                 0.112f},
                                    250, IOSTouchTintStyle::kSlate,
                                    IOSTouchControlShape::kRoundedRect),
      IOSTouchRect{0.385f, 0.025f, 0.100f, 0.052f});
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchActionButton("start_button", IOSTouchAction::kStart,
                                    IOSTouchRect{0.495f, 0.045f, 0.085f,
                                                 0.112f},
                                    250, IOSTouchTintStyle::kSlate,
                                    IOSTouchControlShape::kRoundedRect),
      IOSTouchRect{0.515f, 0.025f, 0.105f, 0.052f});
}

void AddShoulderPair(xe::hid::touch::IOSTouchLayoutModel* layout,
                     float y = 0.055f) {
  using namespace xe::hid::touch;
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchActionButton("left_bumper_button",
                                    IOSTouchAction::kLeftBumper,
                                    IOSTouchRect{0.660f, y, 0.085f, 0.112f},
                                    242, IOSTouchTintStyle::kSky,
                                    IOSTouchControlShape::kRoundedRect),
      IOSTouchRect{0.605f, 0.180f, 0.110f, 0.058f});
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchActionButton("right_bumper_button",
                                    IOSTouchAction::kRightBumper,
                                    IOSTouchRect{0.765f, y, 0.085f, 0.112f},
                                    243, IOSTouchTintStyle::kSky,
                                    IOSTouchControlShape::kRoundedRect),
      IOSTouchRect{0.785f, 0.180f, 0.110f, 0.058f});
}

void AddFaceDiamond(xe::hid::touch::IOSTouchLayoutModel* layout, float center_x,
                    float center_y, float width, float height,
                    uint8_t base_priority = 236) {
  using namespace xe::hid::touch;
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchActionButton("swap_button", IOSTouchAction::kButtonY,
                                    IOSTouchRect{center_x, center_y - height,
                                                 width, height},
                                    base_priority, IOSTouchTintStyle::kAmber),
      IOSTouchRect{0.720f, 0.580f, 0.125f, 0.075f});
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchActionButton("reload_button", IOSTouchAction::kButtonX,
                                    IOSTouchRect{center_x - width * 0.95f,
                                                 center_y, width, height},
                                    base_priority + 1,
                                    IOSTouchTintStyle::kSky),
      IOSTouchRect{0.610f, 0.670f, 0.125f, 0.075f});
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchActionButton("melee_button", IOSTouchAction::kButtonB,
                                    IOSTouchRect{center_x + width * 0.95f,
                                                 center_y, width, height},
                                    base_priority + 2,
                                    IOSTouchTintStyle::kRose),
      IOSTouchRect{0.830f, 0.670f, 0.125f, 0.075f});
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchActionButton("jump_button", IOSTouchAction::kButtonA,
                                    IOSTouchRect{center_x, center_y + height,
                                                 width, height},
                                    base_priority + 3,
                                    IOSTouchTintStyle::kMint),
      IOSTouchRect{0.720f, 0.760f, 0.125f, 0.075f});
}

void AddDpadCross(xe::hid::touch::IOSTouchLayoutModel* layout, float x,
                  float y) {
  using namespace xe::hid::touch;
  constexpr float kDpadWidth = 0.062f;
  constexpr float kDpadHeight = 0.112f;
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchActionButton("dpad_up_button", IOSTouchAction::kDpadUp,
                                    IOSTouchRect{x + kDpadWidth, y,
                                                 kDpadWidth, kDpadHeight},
                                    232, IOSTouchTintStyle::kSlate,
                                    IOSTouchControlShape::kRoundedRect, false,
                                    "Up"),
      IOSTouchRect{0.145f, 0.460f, 0.110f, 0.058f});
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchActionButton("dpad_left_button",
                                    IOSTouchAction::kDpadLeft,
                                    IOSTouchRect{x, y + kDpadHeight,
                                                 kDpadWidth, kDpadHeight},
                                    232, IOSTouchTintStyle::kSlate,
                                    IOSTouchControlShape::kRoundedRect, false,
                                    "Left"),
      IOSTouchRect{0.035f, 0.525f, 0.110f, 0.058f});
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchActionButton("dpad_right_button",
                                    IOSTouchAction::kDpadRight,
                                    IOSTouchRect{x + kDpadWidth * 2.0f,
                                                 y + kDpadHeight, kDpadWidth,
                                                 kDpadHeight},
                                    232, IOSTouchTintStyle::kSlate,
                                    IOSTouchControlShape::kRoundedRect, false,
                                    "Right"),
      IOSTouchRect{0.255f, 0.525f, 0.110f, 0.058f});
  AddOfficialTouchControl(
      layout,
      MakeOfficialTouchActionButton("dpad_down_button",
                                    IOSTouchAction::kDpadDown,
                                    IOSTouchRect{x + kDpadWidth,
                                                 y + kDpadHeight * 2.0f,
                                                 kDpadWidth, kDpadHeight},
                                    232, IOSTouchTintStyle::kSlate,
                                    IOSTouchControlShape::kRoundedRect, false,
                                    "Down"),
      IOSTouchRect{0.145f, 0.590f, 0.110f, 0.058f});
}

xe::hid::touch::IOSTouchLayoutModel MakeOfficialIOSFPSCompactLayoutModel() {
  using namespace xe::hid::touch;
  IOSTouchLayoutModel layout =
      MakeOfficialTouchLayout(kOfficialTouchLayoutFPSCompactLocalID,
                              "FPS Compact");

  IOSTouchControlDefinition move = MakeOfficialTouchMoveStick(
      IOSTouchRect{0.055f, 0.560f, 0.190f, 0.315f}, true);
  move.secondary_behavior =
      MakeSecondaryAction(IOSTouchInteractionTrigger::kDoubleTapForward,
                          IOSTouchAction::kLeftThumb);
  AddOfficialTouchControl(&layout, std::move(move),
                          IOSTouchRect{0.060f, 0.675f, 0.290f, 0.205f});
  AddOfficialTouchControl(&layout, MakeOfficialTouchLookBackdrop(),
                          IOSTouchRect{0.0f, 0.0f, 1.0f, 1.0f});
  AddTopSystemButtons(&layout);
  AddShoulderPair(&layout);
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("aim_button", IOSTouchAction::kLeftTrigger,
                                    IOSTouchRect{0.095f, 0.405f, 0.120f,
                                                 0.110f},
                                    240, IOSTouchTintStyle::kMint,
                                    IOSTouchControlShape::kRoundedRect, true),
      IOSTouchRect{0.080f, 0.575f, 0.170f, 0.070f});
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("fire_button",
                                    IOSTouchAction::kRightTrigger,
                                    IOSTouchRect{0.860f, 0.405f, 0.120f,
                                                 0.110f},
                                    244, IOSTouchTintStyle::kCoral,
                                    IOSTouchControlShape::kRoundedRect, true),
      IOSTouchRect{0.770f, 0.575f, 0.170f, 0.070f});
  AddFaceDiamond(&layout, 0.760f, 0.585f, 0.065f, 0.115f);
  for (auto& control : layout.controls) {
    if (control.identifier == "melee_button") {
      control.secondary_behavior =
          MakeSecondaryAction(IOSTouchInteractionTrigger::kHold,
                              IOSTouchAction::kRightThumb);
      break;
    }
  }
  return layout;
}

xe::hid::touch::IOSTouchLayoutModel MakeOfficialIOSFPSFullLayoutModel() {
  using namespace xe::hid::touch;
  IOSTouchLayoutModel layout =
      MakeOfficialTouchLayout(kOfficialTouchLayoutFPSFullLocalID, "FPS Full");

  IOSTouchControlDefinition move = MakeOfficialTouchMoveStick(
      IOSTouchRect{0.055f, 0.560f, 0.175f, 0.305f});
  move.secondary_behavior =
      MakeSecondaryAction(IOSTouchInteractionTrigger::kDoubleTapForward,
                          IOSTouchAction::kLeftThumb);
  AddOfficialTouchControl(&layout, std::move(move),
                          IOSTouchRect{0.060f, 0.675f, 0.285f, 0.200f});
  AddOfficialTouchControl(&layout, MakeOfficialTouchLookBackdrop(),
                          IOSTouchRect{0.0f, 0.0f, 1.0f, 1.0f});
  AddTopSystemButtons(&layout);
  AddDpadCross(&layout, 0.255f, 0.470f);
  AddShoulderPair(&layout, 0.175f);
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("left_thumb_button",
                                    IOSTouchAction::kLeftThumb,
                                    IOSTouchRect{0.235f, 0.760f, 0.075f,
                                                 0.112f},
                                    235, IOSTouchTintStyle::kSlate,
                                    IOSTouchControlShape::kRoundedRect),
      IOSTouchRect{0.060f, 0.890f, 0.120f, 0.060f});
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("right_thumb_button",
                                    IOSTouchAction::kRightThumb,
                                    IOSTouchRect{0.635f, 0.560f, 0.075f,
                                                 0.112f},
                                    235, IOSTouchTintStyle::kSlate,
                                    IOSTouchControlShape::kRoundedRect),
      IOSTouchRect{0.470f, 0.760f, 0.120f, 0.060f});
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("aim_button", IOSTouchAction::kLeftTrigger,
                                    IOSTouchRect{0.095f, 0.405f, 0.120f,
                                                 0.110f},
                                    240, IOSTouchTintStyle::kMint,
                                    IOSTouchControlShape::kRoundedRect, true),
      IOSTouchRect{0.080f, 0.575f, 0.170f, 0.070f});
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("fire_button",
                                    IOSTouchAction::kRightTrigger,
                                    IOSTouchRect{0.860f, 0.405f, 0.120f,
                                                 0.110f},
                                    244, IOSTouchTintStyle::kCoral,
                                    IOSTouchControlShape::kRoundedRect, true),
      IOSTouchRect{0.770f, 0.575f, 0.170f, 0.070f});
  AddFaceDiamond(&layout, 0.780f, 0.560f, 0.065f, 0.115f);
  return layout;
}

xe::hid::touch::IOSTouchLayoutModel MakeOfficialIOSActionAdventureLayoutModel() {
  using namespace xe::hid::touch;
  IOSTouchLayoutModel layout = MakeOfficialTouchLayout(
      kOfficialTouchLayoutActionAdventureLocalID, "Action / Adventure");

  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchMoveStick(IOSTouchRect{0.060f, 0.585f, 0.195f,
                                              0.305f},
                                 true),
      IOSTouchRect{0.060f, 0.690f, 0.290f, 0.195f});
  AddOfficialTouchControl(&layout, MakeOfficialTouchLookBackdrop(),
                          IOSTouchRect{0.0f, 0.0f, 1.0f, 1.0f});
  AddTopSystemButtons(&layout);
  AddShoulderPair(&layout, 0.185f);
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("aim_button", IOSTouchAction::kLeftTrigger,
                                    IOSTouchRect{0.575f, 0.675f, 0.095f,
                                                 0.120f},
                                    238, IOSTouchTintStyle::kMint,
                                    IOSTouchControlShape::kRoundedRect),
      IOSTouchRect{0.535f, 0.770f, 0.135f, 0.078f});
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("fire_button",
                                    IOSTouchAction::kRightTrigger,
                                    IOSTouchRect{0.920f, 0.675f, 0.075f,
                                                 0.120f},
                                    239, IOSTouchTintStyle::kCoral,
                                    IOSTouchControlShape::kRoundedRect),
      IOSTouchRect{0.850f, 0.770f, 0.135f, 0.078f});
  AddFaceDiamond(&layout, 0.760f, 0.570f, 0.082f, 0.135f);
  return layout;
}

xe::hid::touch::IOSTouchLayoutModel MakeOfficialIOSArcadeDpadLayoutModel() {
  using namespace xe::hid::touch;
  IOSTouchLayoutModel layout = MakeOfficialTouchLayout(
      kOfficialTouchLayoutArcadeDpadLocalID, "Arcade / D-Pad");

  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchMoveStick(IOSTouchRect{0.075f, 0.560f, 0.220f,
                                              0.320f},
                                 true),
      IOSTouchRect{0.075f, 0.675f, 0.310f, 0.210f});
  AddTopSystemButtons(&layout);
  AddShoulderPair(&layout, 0.155f);
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("aim_button", IOSTouchAction::kLeftTrigger,
                                    IOSTouchRect{0.585f, 0.740f, 0.090f,
                                                 0.112f},
                                    236, IOSTouchTintStyle::kMint,
                                    IOSTouchControlShape::kRoundedRect),
      IOSTouchRect{0.535f, 0.785f, 0.135f, 0.075f});
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("fire_button",
                                    IOSTouchAction::kRightTrigger,
                                    IOSTouchRect{0.895f, 0.740f, 0.090f,
                                                 0.112f},
                                    237, IOSTouchTintStyle::kCoral,
                                    IOSTouchControlShape::kRoundedRect),
      IOSTouchRect{0.850f, 0.785f, 0.135f, 0.075f});
  AddFaceDiamond(&layout, 0.760f, 0.560f, 0.085f, 0.135f);
  return layout;
}

xe::hid::touch::IOSTouchLayoutModel MakeOfficialIOSDrivingLayoutModel() {
  using namespace xe::hid::touch;
  IOSTouchLayoutModel layout =
      MakeOfficialTouchLayout(kOfficialTouchLayoutDrivingLocalID, "Driving");

  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchMoveStick(IOSTouchRect{0.065f, 0.560f, 0.195f,
                                              0.315f},
                                 false, 220, "Steer"),
      IOSTouchRect{0.065f, 0.680f, 0.285f, 0.200f});
  AddTopSystemButtons(&layout);
  AddShoulderPair(&layout, 0.140f);
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("brake_button",
                                    IOSTouchAction::kLeftTrigger,
                                    IOSTouchRect{0.580f, 0.620f, 0.170f,
                                                 0.180f},
                                    240, IOSTouchTintStyle::kMint,
                                    IOSTouchControlShape::kRoundedRect),
      IOSTouchRect{0.530f, 0.760f, 0.190f, 0.095f});
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("throttle_button",
                                    IOSTouchAction::kRightTrigger,
                                    IOSTouchRect{0.790f, 0.620f, 0.170f,
                                                 0.180f},
                                    244, IOSTouchTintStyle::kCoral,
                                    IOSTouchControlShape::kRoundedRect),
      IOSTouchRect{0.770f, 0.760f, 0.190f, 0.095f});
  AddFaceDiamond(&layout, 0.760f, 0.390f, 0.064f, 0.112f, 232);
  return layout;
}

xe::hid::touch::IOSTouchLayoutModel MakeOfficialIOSMinimalStarterLayoutModel() {
  using namespace xe::hid::touch;
  IOSTouchLayoutModel layout = MakeOfficialTouchLayout(
      kOfficialTouchLayoutMinimalStarterLocalID, "Minimal Starter");

  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchMoveStick(IOSTouchRect{0.070f, 0.600f, 0.190f,
                                              0.300f},
                                 true),
      IOSTouchRect{0.070f, 0.690f, 0.285f, 0.195f});
  AddOfficialTouchControl(&layout, MakeOfficialTouchLookBackdrop(),
                          IOSTouchRect{0.0f, 0.0f, 1.0f, 1.0f});
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchPauseButton(IOSTouchRect{0.040f, 0.050f, 0.110f,
                                                0.112f}),
      IOSTouchRect{0.055f, 0.025f, 0.180f, 0.058f});
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("aim_button", IOSTouchAction::kLeftTrigger,
                                    IOSTouchRect{0.610f, 0.710f, 0.110f,
                                                 0.130f},
                                    240, IOSTouchTintStyle::kMint,
                                    IOSTouchControlShape::kRoundedRect, true),
      IOSTouchRect{0.535f, 0.770f, 0.145f, 0.080f});
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("reload_button", IOSTouchAction::kButtonX,
                                    IOSTouchRect{0.710f, 0.560f, 0.070f,
                                                 0.120f},
                                    236, IOSTouchTintStyle::kSky),
      IOSTouchRect{0.640f, 0.675f, 0.125f, 0.075f});
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("jump_button", IOSTouchAction::kButtonA,
                                    IOSTouchRect{0.780f, 0.705f, 0.070f,
                                                 0.120f},
                                    238, IOSTouchTintStyle::kMint),
      IOSTouchRect{0.760f, 0.760f, 0.125f, 0.075f});
  AddOfficialTouchControl(
      &layout,
      MakeOfficialTouchActionButton("fire_button",
                                    IOSTouchAction::kRightTrigger,
                                    IOSTouchRect{0.875f, 0.700f, 0.110f,
                                                 0.135f},
                                    244, IOSTouchTintStyle::kCoral,
                                    IOSTouchControlShape::kRoundedRect, true),
      IOSTouchRect{0.840f, 0.770f, 0.145f, 0.080f});
  return layout;
}

const IOSTouchOfficialLayoutPreset kOfficialTouchLayoutPresets[] = {
    {kOfficialTouchLayoutFPSCompactLocalID, "FPS Compact",
     &MakeOfficialIOSFPSCompactLayoutModel},
    {kOfficialTouchLayoutFPSFullLocalID, "FPS Full",
     &MakeOfficialIOSFPSFullLayoutModel},
    {kOfficialTouchLayoutActionAdventureLocalID, "Action / Adventure",
     &MakeOfficialIOSActionAdventureLayoutModel},
    {kOfficialTouchLayoutArcadeDpadLocalID, "Arcade / D-Pad",
     &MakeOfficialIOSArcadeDpadLayoutModel},
    {kOfficialTouchLayoutDrivingLocalID, "Driving",
     &MakeOfficialIOSDrivingLayoutModel},
    {kOfficialTouchLayoutMinimalStarterLocalID, "Minimal Starter",
     &MakeOfficialIOSMinimalStarterLayoutModel},
};

NSString* const kXeniaTouchLayoutGlobalDefaultLocalIDKey =
    @"XeniaTouchLayoutGlobalDefaultLocalID";
NSString* const kXeniaTouchLayoutFavoriteLocalIDsKey =
    @"XeniaTouchLayoutFavoriteLocalIDs";

const IOSTouchOfficialLayoutPreset* FindOfficialTouchLayoutPreset(
    const std::string& local_id) {
  for (const auto& preset : kOfficialTouchLayoutPresets) {
    if (local_id == preset.local_id) {
      return &preset;
    }
  }
  return nullptr;
}

bool IsObsoleteOfficialTouchLayoutLocalID(const std::string& local_id) {
  return local_id == "fps_standard" || local_id == "fps_ipad" ||
         local_id == "fps_expanded" || local_id == "fps_mirrored";
}

}  // namespace

bool IsOfficialTouchLayoutLocalID(const std::string& local_id) {
  return FindOfficialTouchLayoutPreset(local_id) != nullptr;
}

size_t OfficialTouchLayoutPresetSortOrder(const std::string& local_id) {
  for (size_t index = 0; index < std::size(kOfficialTouchLayoutPresets);
       ++index) {
    if (local_id == kOfficialTouchLayoutPresets[index].local_id) {
      return index;
    }
  }
  return std::size(kOfficialTouchLayoutPresets);
}

std::string NormalizeOfficialTouchLayoutBaseTemplate(
    std::string base_template) {
  if (FindOfficialTouchLayoutPreset(base_template)) {
    return base_template;
  }
  return DefaultOfficialTouchLayoutLocalID();
}

std::string MakeTouchLayoutSlug(std::string value) {
  std::string slug;
  slug.reserve(value.size());
  bool last_was_separator = false;
  for (char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      slug.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      last_was_separator = false;
      continue;
    }
    if (!last_was_separator && !slug.empty()) {
      slug.push_back('_');
      last_was_separator = true;
    }
  }
  while (!slug.empty() && slug.back() == '_') {
    slug.pop_back();
  }
  return slug.empty() ? "touch_layout" : slug;
}

bool TryNormalizeConfiguredTouchLayoutLocalID(
    const std::string& configured_local_id,
    std::string* normalized_local_id_out) {
  if (!normalized_local_id_out || configured_local_id.empty()) {
    return false;
  }
  if (configured_local_id.find('/') != std::string::npos ||
      configured_local_id.find('\\') != std::string::npos ||
      configured_local_id.find("..") != std::string::npos) {
    return false;
  }
  std::string normalized_local_id = MakeTouchLayoutSlug(configured_local_id);
  if (normalized_local_id.empty() ||
      normalized_local_id != configured_local_id) {
    return false;
  }
  if (IsObsoleteOfficialTouchLayoutLocalID(normalized_local_id)) {
    return false;
  }
  *normalized_local_id_out = std::move(normalized_local_id);
  return true;
}

std::string TouchLayoutBaseTemplateForTable(const toml::table& table) {
  if (auto base_template = table["base_template"].value<std::string>()) {
    return NormalizeOfficialTouchLayoutBaseTemplate(*base_template);
  }
  if (auto layout_id = table["layout_id"].value<std::string>()) {
    return NormalizeOfficialTouchLayoutBaseTemplate(*layout_id);
  }
  return DefaultOfficialTouchLayoutLocalID();
}

std::string DefaultOfficialTouchLayoutLocalID() {
  return UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad
             ? kOfficialTouchLayoutFPSFullLocalID
             : kOfficialTouchLayoutFPSCompactLocalID;
}

xe::hid::touch::IOSTouchLayoutModel MakeTouchLayoutSeedModelForTable(
    const toml::table& table) {
  return MakeOfficialIOSTouchLayoutModelForLocalID(
      TouchLayoutBaseTemplateForTable(table));
}

xe::hid::touch::IOSTouchLayoutModel MakeOfficialIOSTouchLayoutModelForLocalID(
    const std::string& local_id) {
  if (const auto* preset = FindOfficialTouchLayoutPreset(local_id)) {
    return preset->factory();
  }
  return MakeOfficialIOSFPSCompactLayoutModel();
}

xe::hid::touch::IOSTouchLayoutModel MakeOfficialIOSTouchLayoutModel() {
  return MakeOfficialIOSTouchLayoutModelForLocalID(
      DefaultOfficialTouchLayoutLocalID());
}

UIImage* RenderTouchLayoutThumbnail(
    const xe::hid::touch::IOSTouchLayoutModel& layout, CGSize size) {
  if (size.width <= 0.0 || size.height <= 0.0) {
    return nil;
  }
  UIGraphicsImageRendererFormat* format =
      [UIGraphicsImageRendererFormat preferredFormat];
  format.opaque = NO;
  format.scale = 0.0;
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:size format:format]
          autorelease];
  return [renderer imageWithActions:^(UIGraphicsImageRendererContext* ctx) {
    CGContextRef cg = ctx.CGContext;
    CGContextSetFillColorWithColor(
        cg, [UIColor colorWithWhite:0.10 alpha:0.95].CGColor);
    CGContextFillRect(cg, CGRectMake(0, 0, size.width, size.height));

    auto draw_control = ^(const xe::hid::touch::IOSTouchControlDefinition& control,
                          CGFloat fill_alpha, CGFloat stroke_alpha) {
      const CGRect frame = CGRectMake(
          control.normalized_frame.x * size.width,
          control.normalized_frame.y * size.height,
          MAX(control.normalized_frame.width * size.width, 2.0),
          MAX(control.normalized_frame.height * size.height, 2.0));
      UIColor* tint =
          XeniaTouchOverlayAccentColor(control.tint_style, control.type);
      CGContextSetFillColorWithColor(
          cg, [tint colorWithAlphaComponent:fill_alpha].CGColor);
      CGContextSetStrokeColorWithColor(
          cg, [tint colorWithAlphaComponent:stroke_alpha].CGColor);
      CGContextSetLineWidth(cg, 0.5);
      const CGFloat corner =
          control.shape == xe::hid::touch::IOSTouchControlShape::kCircle
              ? MIN(frame.size.width, frame.size.height) * 0.5
              : MIN(MIN(frame.size.width, frame.size.height) * 0.30, 4.0);
      UIBezierPath* path =
          [UIBezierPath bezierPathWithRoundedRect:frame cornerRadius:corner];
      [path fill];
      [path stroke];
    };

    for (const auto& control : layout.controls) {
      if (control.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone &&
          control.visual_opacity <= 0.01f) {
        draw_control(control, 0.08, 0.20);
      }
    }

    for (const auto& control : layout.controls) {
      if (control.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone &&
          control.visual_opacity <= 0.01f) {
        continue;
      }
      const CGFloat opacity =
          static_cast<CGFloat>(std::clamp(control.visual_opacity, 0.15f, 1.0f));
      draw_control(control, 0.18 + 0.40 * opacity, 0.40 + 0.55 * opacity);
    }
  }];
}

std::string ReadGlobalTouchLayoutAssignment() {
  NSString* value = [[NSUserDefaults standardUserDefaults]
      stringForKey:kXeniaTouchLayoutGlobalDefaultLocalIDKey];
  if (!value.length) {
    return std::string();
  }
  std::string normalized;
  if (!TryNormalizeConfiguredTouchLayoutLocalID(std::string([value UTF8String]),
                                                &normalized)) {
    return std::string();
  }
  return normalized;
}

void WriteGlobalTouchLayoutAssignment(const std::string& local_id) {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if (local_id.empty()) {
    [defaults removeObjectForKey:kXeniaTouchLayoutGlobalDefaultLocalIDKey];
    return;
  }
  std::string normalized;
  if (!TryNormalizeConfiguredTouchLayoutLocalID(local_id, &normalized)) {
    return;
  }
  [defaults setObject:[NSString stringWithUTF8String:normalized.c_str()]
               forKey:kXeniaTouchLayoutGlobalDefaultLocalIDKey];
}

std::string ReadTitleTouchLayoutAssignment(uint32_t title_id) {
  if (!title_id) {
    return std::string();
  }
  toml::table config = config::LoadGameConfig(title_id);
  const toml::table* assignment =
      config[kTouchLayoutAssignmentSection].as_table();
  if (!assignment) {
    return std::string();
  }
  auto local_layout_id = (*assignment)["local_layout_id"].value<std::string>();
  if (!local_layout_id) {
    return std::string();
  }
  std::string normalized;
  if (!TryNormalizeConfiguredTouchLayoutLocalID(*local_layout_id,
                                                &normalized)) {
    return std::string();
  }
  return normalized;
}

bool IsFavoriteTouchLayoutLocalID(const std::string& local_id) {
  std::string normalized;
  if (!TryNormalizeConfiguredTouchLayoutLocalID(local_id, &normalized)) {
    return false;
  }
  NSArray<NSString*>* favorites = [[NSUserDefaults standardUserDefaults]
      stringArrayForKey:kXeniaTouchLayoutFavoriteLocalIDsKey];
  for (NSString* favorite in favorites) {
    if ([favorite isEqualToString:[NSString stringWithUTF8String:normalized.c_str()]]) {
      return true;
    }
  }
  return false;
}

void SetFavoriteTouchLayoutLocalID(const std::string& local_id, bool favorite) {
  std::string normalized;
  if (!TryNormalizeConfiguredTouchLayoutLocalID(local_id, &normalized)) {
    return;
  }
  NSString* favorite_id = [NSString stringWithUTF8String:normalized.c_str()];
  NSArray<NSString*>* stored_favorites = [[NSUserDefaults standardUserDefaults]
      stringArrayForKey:kXeniaTouchLayoutFavoriteLocalIDsKey];
  NSArray<NSString*>* favorite_source =
      stored_favorites ? stored_favorites : [NSArray array];
  NSMutableOrderedSet<NSString*>* favorites =
      [NSMutableOrderedSet orderedSetWithArray:favorite_source];
  if (favorite) {
    [favorites addObject:favorite_id];
  } else {
    [favorites removeObject:favorite_id];
  }
  [[NSUserDefaults standardUserDefaults] setObject:favorites.array
                                            forKey:kXeniaTouchLayoutFavoriteLocalIDsKey];
}

bool TouchLayoutContentMatches(
    const xe::hid::touch::IOSTouchLayoutModel& a,
    const xe::hid::touch::IOSTouchLayoutModel& b) {
  std::ostringstream stream_a;
  std::ostringstream stream_b;
  stream_a << xe::hid::touch::EncodeIOSTouchLayoutModel(a);
  stream_b << xe::hid::touch::EncodeIOSTouchLayoutModel(b);
  return stream_a.str() == stream_b.str();
}

}  // namespace ui
}  // namespace xe
