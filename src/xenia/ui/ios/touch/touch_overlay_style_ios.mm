/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/touch/touch_overlay_style_ios.h"

#include <algorithm>

#import "xenia/ui/ios/shared/ios_theme.h"

namespace xe {
namespace ui {

UIColor* XeniaTouchOverlayAccentColor(
    xe::hid::touch::IOSTouchTintStyle tint_style,
    xe::hid::touch::IOSTouchControlType type) {
  switch (tint_style) {
    case xe::hid::touch::IOSTouchTintStyle::kAmber:
      return [XeniaTheme touchTintAmber];
    case xe::hid::touch::IOSTouchTintStyle::kSky:
      return [XeniaTheme touchTintSky];
    case xe::hid::touch::IOSTouchTintStyle::kMint:
      return [XeniaTheme touchTintMint];
    case xe::hid::touch::IOSTouchTintStyle::kRose:
      return [XeniaTheme touchTintRose];
    case xe::hid::touch::IOSTouchTintStyle::kLime:
      return [XeniaTheme touchTintLime];
    case xe::hid::touch::IOSTouchTintStyle::kCoral:
      return [XeniaTheme touchTintCoral];
    case xe::hid::touch::IOSTouchTintStyle::kSlate:
      return [XeniaTheme touchTintSlate];
    case xe::hid::touch::IOSTouchTintStyle::kAuto:
    default:
      if (type == xe::hid::touch::IOSTouchControlType::kPauseButton) {
        return [XeniaTheme touchTintAmber];
      }
      return [UIColor colorWithWhite:1.0 alpha:1.0];
  }
}

UIColor* XeniaTouchOverlayBorderColorForControl(
    const xe::hid::touch::IOSTouchControlDefinition& control) {
  UIColor* accent_color =
      XeniaTouchOverlayAccentColor(control.tint_style, control.type);
  switch (control.type) {
    case xe::hid::touch::IOSTouchControlType::kPauseButton:
      return [accent_color colorWithAlphaComponent:0.95];
    case xe::hid::touch::IOSTouchControlType::kMoveStick:
      return [accent_color colorWithAlphaComponent:0.38];
    case xe::hid::touch::IOSTouchControlType::kLookSwipeZone:
      return [accent_color colorWithAlphaComponent:0.12];
    case xe::hid::touch::IOSTouchControlType::kActionButton:
    default:
      return [accent_color colorWithAlphaComponent:0.32];
  }
}

UIColor* XeniaTouchOverlayFillColorForControl(
    const xe::hid::touch::IOSTouchControlDefinition& control, float opacity) {
  const CGFloat alpha =
      static_cast<CGFloat>(std::clamp(opacity, 0.0f, 1.0f));
  UIColor* accent_color =
      XeniaTouchOverlayAccentColor(control.tint_style, control.type);
  switch (control.type) {
    case xe::hid::touch::IOSTouchControlType::kPauseButton:
      return [accent_color colorWithAlphaComponent:0.18f * alpha];
    case xe::hid::touch::IOSTouchControlType::kMoveStick:
      return [accent_color colorWithAlphaComponent:0.10f * alpha];
    case xe::hid::touch::IOSTouchControlType::kLookSwipeZone:
      return [UIColor clearColor];
    case xe::hid::touch::IOSTouchControlType::kActionButton:
    default:
      return [accent_color colorWithAlphaComponent:0.08f * alpha];
  }
}

NSString* XeniaTouchNSStringFromStdString(const std::string& value) {
  return value.empty() ? @"" : [NSString stringWithUTF8String:value.c_str()];
}

NSString* XeniaTouchConfiguredControlLabelText(
    const xe::hid::touch::IOSTouchControlDefinition& control,
    BOOL fallback_to_identifier) {
  std::string label = xe::hid::touch::IOSTouchConfiguredControlLabel(control);
  if (label.empty() && fallback_to_identifier) {
    label = control.identifier;
  }
  return XeniaTouchNSStringFromStdString(label);
}

NSString* XeniaTouchVisibleControlLabelText(
    const xe::hid::touch::IOSTouchControlDefinition& control,
    BOOL fallback_to_identifier) {
  std::string label = xe::hid::touch::IOSTouchVisibleControlLabel(control);
  if (label.empty() && fallback_to_identifier) {
    label = control.identifier;
  }
  return XeniaTouchNSStringFromStdString(label);
}

}  // namespace ui
}  // namespace xe
