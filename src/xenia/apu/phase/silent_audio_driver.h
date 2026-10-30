/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SILENT_AUDIO_DRIVER_H_
#define XENIA_APU_SILENT_AUDIO_DRIVER_H_

#include "xenia/apu/audio_driver.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {

// A driver that produces no audio output but still honors the audio worker
// backpressure contract: every SubmitFrame releases one semaphore permit so the
// guest callback flow keeps advancing. Used as a fallback when a real output
// device can't be created (e.g. secondary clients on platforms that only
// support a single shared output device).
class SilentAudioDriver final : public AudioDriver {
 public:
  explicit SilentAudioDriver(xe::threading::Semaphore* semaphore)
      : semaphore_(semaphore) {}

  bool Initialize() override { return true; }
  void Shutdown() override {}
  void SubmitFrame(float* samples) override {
    (void)samples;
    if (semaphore_) {
      semaphore_->Release(1, nullptr);
    }
  }
  void Pause() override {}
  void Resume() override {}
  void SetVolume(float volume) override { (void)volume; }

 private:
  xe::threading::Semaphore* semaphore_ = nullptr;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_SILENT_AUDIO_DRIVER_H_
