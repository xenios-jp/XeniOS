/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_TOUCH_LAYOUT_EDITOR_H_
#define XENIA_HID_TOUCH_LAYOUT_EDITOR_H_

#include <cstddef>
#include <string>

#include "xenia/hid/touch/touch_layout_ios.h"

namespace xe {
namespace hid {
namespace touch {

bool IOSTouchLayoutHasActionBinding(const IOSTouchLayoutModel& layout,
                                    IOSTouchAction action);
std::string MakeUniqueIOSTouchActionButtonIdentifier(
    const IOSTouchLayoutModel& layout);
IOSTouchAction SuggestedNewIOSTouchActionButtonBinding(
    const IOSTouchLayoutModel& layout);
IOSTouchRect SuggestedNewIOSTouchActionButtonFrame(IOSTouchAction action);
IOSTouchRect ClampIOSTouchEditorControlFrame(const IOSTouchRect& rect,
                                             IOSTouchControlType control_type);
IOSTouchRect FindAvailableIOSTouchEditorControlFrame(
    const IOSTouchLayoutModel& layout, const IOSTouchRect& preferred_frame,
    IOSTouchControlType control_type, bool is_portrait);
bool AddSuggestedActionButtonToIOSTouchLayout(
    IOSTouchLayoutModel* layout, bool is_portrait,
    std::string* selected_identifier_out);
bool MirrorIOSTouchLayoutControlHorizontally(IOSTouchLayoutModel* layout,
                                             std::size_t control_index,
                                             bool is_portrait);
bool CopyIOSTouchLayoutFramesAcrossOrientations(IOSTouchLayoutModel* layout,
                                                bool from_landscape);
bool DuplicateIOSTouchLayoutActionButton(IOSTouchLayoutModel* layout,
                                         std::size_t source_control_index,
                                         bool is_portrait,
                                         std::string* selected_identifier_out);
bool DeleteIOSTouchLayoutControl(IOSTouchLayoutModel* layout,
                                 std::size_t control_index);

}  // namespace touch
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_TOUCH_LAYOUT_EDITOR_H_
