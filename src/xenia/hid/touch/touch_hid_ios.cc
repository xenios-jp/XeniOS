/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/touch/touch_hid_ios.h"

#include "xenia/hid/sdl/sdl_hid.h"
#include "xenia/hid/touch/touch_input_driver_ios.h"

namespace xe {
namespace hid {
namespace touch {

namespace {

// Present touch as a fallback device only while no physical pad is present.
// Edge's normal hotplug reconciliation then moves player one between them.
class IOSInputDriver final : public InputDriver {
 public:
  IOSInputDriver(ui::Window* window, size_t z_order)
      : InputDriver(window, z_order),
        touch_(std::make_unique<TouchInputDriver>(window, z_order)),
        physical_(sdl::Create(window, z_order)) {}

  X_STATUS Setup() override {
    if (physical_ && XFAILED(physical_->Setup())) {
      physical_.reset();
    }
    if (physical_) {
      physical_->set_devices_changed_callback(
          [this]() { NotifyDevicesChanged(); });
    }
    return touch_->Setup();
  }

  InputType GetInputType() const override { return InputType::Controller; }

  std::vector<InputDeviceInfo> EnumerateDevices() override {
    auto devices = physical_ ? physical_->EnumerateDevices()
                             : std::vector<InputDeviceInfo>{};
    if (devices.empty()) {
      devices = touch_->EnumerateDevices();
      for (auto& device : devices) {
        device.driver_slot = kTouchSlot;
      }
    }
    return devices;
  }

  X_RESULT GetCapabilities(uint32_t slot, uint32_t flags,
                           X_INPUT_CAPABILITIES* caps) override {
    return slot == kTouchSlot
               ? touch_->GetCapabilities(0, flags, caps)
               : (physical_ ? physical_->GetCapabilities(slot, flags, caps)
                            : X_ERROR_DEVICE_NOT_CONNECTED);
  }
  X_RESULT GetState(uint32_t slot, X_INPUT_STATE* state) override {
    return slot == kTouchSlot ? touch_->GetState(0, state)
                              : (physical_ ? physical_->GetState(slot, state)
                                           : X_ERROR_DEVICE_NOT_CONNECTED);
  }
  X_RESULT SetState(uint32_t slot, X_INPUT_VIBRATION* vibration) override {
    return slot == kTouchSlot
               ? touch_->SetState(0, vibration)
               : (physical_ ? physical_->SetState(slot, vibration)
                            : X_ERROR_DEVICE_NOT_CONNECTED);
  }
  X_RESULT GetKeystroke(uint32_t slot, uint32_t flags,
                        X_INPUT_KEYSTROKE* key) override {
    return slot == kTouchSlot
               ? touch_->GetKeystroke(0, flags, key)
               : (physical_ ? physical_->GetKeystroke(slot, flags, key)
                            : X_ERROR_DEVICE_NOT_CONNECTED);
  }
  void OnBoundToSlot(uint8_t slot, uint32_t guest_slot) override {
    if (slot != kTouchSlot && physical_) {
      physical_->OnBoundToSlot(slot, guest_slot);
    }
  }
  void OnUnboundFromSlot(uint8_t slot) override {
    if (slot != kTouchSlot && physical_) {
      physical_->OnUnboundFromSlot(slot);
    }
  }

 private:
  static constexpr uint8_t kTouchSlot = 0xFF;
  std::unique_ptr<InputDriver> touch_;
  std::unique_ptr<InputDriver> physical_;
};

}  // namespace

std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order) {
  return std::make_unique<IOSInputDriver>(window, window_z_order);
}

}  // namespace touch
}  // namespace hid
}  // namespace xe
