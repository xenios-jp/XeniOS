/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/touch/touch_layout_ios.h"

namespace xe {
namespace hid {
namespace touch {

IOSTouchRect ResolveIOSTouchRect(const IOSTouchRect& normalized_rect,
                                 const IOSTouchLayoutSpace& safe_area) {
  if (safe_area.IsEmpty()) {
    return IOSTouchRect{};
  }
  return IOSTouchRect{
      safe_area.origin_x + normalized_rect.x * safe_area.width,
      safe_area.origin_y + normalized_rect.y * safe_area.height,
      normalized_rect.width * safe_area.width,
      normalized_rect.height * safe_area.height,
  };
}

bool IOSTouchRectContainsPoint(const IOSTouchRect& rect,
                               const IOSTouchPoint& point) {
  const float rect_max_x = rect.x + rect.width;
  const float rect_max_y = rect.y + rect.height;
  return point.x >= rect.x && point.x <= rect_max_x && point.y >= rect.y &&
         point.y <= rect_max_y;
}

const IOSTouchRect& ActiveControlFrameForOrientation(
    const IOSTouchControlDefinition& control, bool is_portrait) {
  if (is_portrait && control.has_portrait_frame) {
    return control.portrait_normalized_frame;
  }
  return control.normalized_frame;
}

IOSTouchRect& MutableActiveControlFrameForOrientation(
    IOSTouchControlDefinition& control, bool is_portrait) {
  if (!is_portrait) {
    return control.normalized_frame;
  }
  if (!control.has_portrait_frame) {
    // Lazily promote: seed the portrait frame from the current landscape
    // baseline so the very first portrait edit nudges from a sensible
    // starting point instead of (0, 0). Callers that explicitly want the
    // portrait override empty (e.g. "Copy Layout from Landscape") set the
    // fields directly rather than going through this helper.
    control.portrait_normalized_frame = control.normalized_frame;
    control.has_portrait_frame = true;
  }
  return control.portrait_normalized_frame;
}

}  // namespace touch
}  // namespace hid
}  // namespace xe
