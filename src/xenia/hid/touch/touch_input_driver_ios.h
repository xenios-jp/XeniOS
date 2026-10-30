/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_TOUCH_INPUT_DRIVER_IOS_H_
#define XENIA_HID_TOUCH_INPUT_DRIVER_IOS_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "xenia/hid/input_driver.h"

namespace xe {
namespace hid {
namespace touch {
class IOSTouchRuntimeModel;
}  // namespace touch
}  // namespace hid
}  // namespace xe

namespace xe {
namespace hid {
namespace touch {

// HID input driver that surfaces the iOS on-screen touch overlay's resolved
// gamepad state to the emulator.
//
// Lifetime contract for `runtime_model_`:
//   * The IOSTouchRuntimeModel is owned by XeniaViewController on the UIKit
//     main thread (see ios_main_view_controller.mm) and lives for the
//     controller's lifetime, which spans the whole app process.
//   * IOSWindowedAppContext::set_touch_runtime_model() is invoked exactly
//     once by XeniaAppDelegate after the controller is created. The pointer
//     is never re-set or cleared while the driver is alive.
//   * RefreshRuntimeModel() lazily caches the pointer on first use; once
//     cached it never changes. GetState/GetKeystroke read the resolved-state
//     buffer through this pointer; the buffer itself is internally
//     mutex-protected, so cross-thread reads are safe.
//   * If the controller were ever to outlive the driver, dropping the cached
//     pointer to nullptr would also be safe — every consumer re-checks via
//     RefreshRuntimeModel(). The reverse (driver outliving the controller)
//     would be a use-after-free; in the current architecture it cannot happen
//     because the emulator (which owns the driver) is torn down before the
//     UIKit hierarchy.
class TouchInputDriver final : public InputDriver {
 public:
  explicit TouchInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~TouchInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  InputType GetInputType() const override;
  std::vector<InputDeviceInfo> EnumerateDevices() override;

 private:
  bool IsUserSupported(uint32_t user_index) const;
  bool RefreshRuntimeModel();

  IOSTouchRuntimeModel* runtime_model_ = nullptr;
  std::mutex keystroke_mutex_;
  bool has_keystroke_state_ = false;
  uint64_t last_keystroke_field_ = 0;
  std::deque<X_INPUT_KEYSTROKE> pending_keystrokes_;
};

}  // namespace touch
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_TOUCH_INPUT_DRIVER_IOS_H_
