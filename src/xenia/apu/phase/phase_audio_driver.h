/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_PHASE_PHASE_AUDIO_DRIVER_H_
#define XENIA_APU_PHASE_PHASE_AUDIO_DRIVER_H_

#include <cstdint>
#include <memory>

#include "xenia/apu/audio_driver.h"

namespace xe {
namespace threading {
class Semaphore;
}  // namespace threading
}  // namespace xe

namespace xe {
namespace apu {
namespace phase {

// Audio driver that renders the guest 5.1 bed through Apple's PHASE engine for
// native binaural/spatial output. The implementation lives in the .mm file and
// is hidden behind an opaque Impl so this header stays free of Objective-C and
// can be included from plain C++ translation units (e.g. the audio system).
//
// NOTE: This is currently the Phase 1 scaffold — it compiles and links the
// PHASE framework and honors the worker backpressure contract, but produces
// silence until the Phase 2 playback path is implemented.
class PHASEAudioDriver final : public AudioDriver {
 public:
  explicit PHASEAudioDriver(
      xe::threading::Semaphore* semaphore,
      uint32_t frequency = kFrameFrequencyDefault,
      uint32_t channels = kFrameChannelsDefault);
  ~PHASEAudioDriver() override;

  bool Initialize() override;
  void Shutdown() override;
  void SubmitFrame(float* samples) override;
  void Pause() override;
  void Resume() override;
  void SetVolume(float volume) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace phase
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_PHASE_PHASE_AUDIO_DRIVER_H_
