/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_OVERLAY_STYLE_IOS_H_
#define XENIA_UI_IOS_TOUCH_OVERLAY_STYLE_IOS_H_

#import <UIKit/UIKit.h>

#include <string>

#include "xenia/hid/touch/touch_layout_ios.h"

namespace xe {
namespace ui {

constexpr CGFloat kXeniaTouchComboStickRadiusFraction = 0.32f;

UIColor* XeniaTouchOverlayAccentColor(xe::hid::touch::IOSTouchTintStyle tint_style,
                                      xe::hid::touch::IOSTouchControlType type);
UIColor* XeniaTouchOverlayBorderColorForControl(
    const xe::hid::touch::IOSTouchControlDefinition& control);
UIColor* XeniaTouchOverlayFillColorForControl(
    const xe::hid::touch::IOSTouchControlDefinition& control, float opacity);
NSString* XeniaTouchNSStringFromStdString(const std::string& value);
NSString* XeniaTouchConfiguredControlLabelText(
    const xe::hid::touch::IOSTouchControlDefinition& control, BOOL fallback_to_identifier);
NSString* XeniaTouchVisibleControlLabelText(
    const xe::hid::touch::IOSTouchControlDefinition& control, BOOL fallback_to_identifier);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_TOUCH_OVERLAY_STYLE_IOS_H_
