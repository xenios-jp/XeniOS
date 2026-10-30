/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_TOUCH_OVERLAY_EDIT_HISTORY_IOS_H_
#define XENIA_UI_IOS_TOUCH_TOUCH_OVERLAY_EDIT_HISTORY_IOS_H_

#include <cstddef>
#include <string>
#include <vector>

#include "xenia/hid/touch/touch_layout_ios.h"

namespace xe::ui::ios::touch_overlay {

class TouchOverlayEditHistoryIOS {
 public:
  void Reset();
  void SeedIfNeeded(const xe::hid::touch::IOSTouchLayoutModel& layout,
                    std::string selected_control_identifier);
  void BeginChange(const xe::hid::touch::IOSTouchLayoutModel& layout,
                   std::string selected_control_identifier);
  bool FinishChange(const xe::hid::touch::IOSTouchLayoutModel& layout,
                    std::string selected_control_identifier);
  void CancelChange();

  bool IsChangeActive() const { return change_active_; }
  bool CanUndo() const;
  bool CanRedo() const;
  bool Undo(xe::hid::touch::IOSTouchLayoutModel* layout,
            std::string* selected_control_identifier);
  bool Redo(xe::hid::touch::IOSTouchLayoutModel* layout,
            std::string* selected_control_identifier);

 private:
  struct Entry {
    xe::hid::touch::IOSTouchLayoutModel layout;
    std::string selected_control_identifier;
  };

  static bool EntriesEqual(const Entry& left, const Entry& right);
  void Trim();
  void PushUndoEntry(Entry entry);

  bool change_active_ = false;
  Entry pending_change_;
  std::vector<Entry> undo_history_;
  std::vector<Entry> redo_history_;
};

}  // namespace xe::ui::ios::touch_overlay

#endif  // XENIA_UI_IOS_TOUCH_TOUCH_OVERLAY_EDIT_HISTORY_IOS_H_
