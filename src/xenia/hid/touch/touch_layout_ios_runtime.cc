/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/touch/touch_layout_ios.h"

#include <memory>
#include <utility>

namespace xe {
namespace hid {
namespace touch {

void IOSTouchResolvedStateBuffer::Store(const IOSTouchResolvedState& state) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = state;
}

IOSTouchResolvedState IOSTouchResolvedStateBuffer::Load() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

IOSTouchRuntimeModel::IOSTouchRuntimeModel()
    : layout_(CreateDefaultIOSFPSLayoutModel()) {
  PublishControlsSnapshot();
}

void IOSTouchRuntimeModel::SetLayout(IOSTouchLayoutModel layout) {
  layout_ = std::move(layout);
  PublishControlsSnapshot();
}

void IOSTouchRuntimeModel::PublishControlsSnapshot() {
  auto snapshot = std::make_shared<const std::vector<IOSTouchControlDefinition>>(
      layout_.controls);
  std::lock_guard<std::mutex> lock(controls_snapshot_mutex_);
  controls_snapshot_ = std::move(snapshot);
}

std::shared_ptr<const std::vector<IOSTouchControlDefinition>>
IOSTouchRuntimeModel::LoadControlsSnapshot() const {
  std::lock_guard<std::mutex> lock(controls_snapshot_mutex_);
  return controls_snapshot_;
}

void IOSTouchRuntimeModel::StoreResolvedState(
    const IOSTouchResolvedState& state) {
  resolved_state_.Store(state);
}

IOSTouchResolvedState IOSTouchRuntimeModel::LoadResolvedState() const {
  return resolved_state_.Load();
}

void IOSTouchRuntimeModel::ResetResolvedState() {
  resolved_state_.Store(IOSTouchResolvedState{});
}

}  // namespace touch
}  // namespace hid
}  // namespace xe
