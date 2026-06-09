/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_PHASE_PHASE_AUDIO_SYSTEM_H_
#define XENIA_APU_PHASE_PHASE_AUDIO_SYSTEM_H_

#include "xenia/apu/audio_system.h"

namespace xe {
namespace apu {
namespace phase {

// Apple-platforms audio system that renders the main 5.1 system through PHASE
// for native spatial audio. Mirrors SDLAudioSystem: a single real driver backs
// guest client 0; all other clients and the independent/XMP path fall back to a
// silent driver (one physical output; multiple PHASE engines would contend).
class PHASEAudioSystem : public AudioSystem {
 public:
  explicit PHASEAudioSystem(cpu::Processor* processor);
  ~PHASEAudioSystem() override;

  static bool IsAvailable() { return true; }

  static std::unique_ptr<AudioSystem> Create(cpu::Processor* processor);

  std::string name() const override { return "PHASE"; }

  X_RESULT CreateDriver(size_t index, xe::threading::Semaphore* semaphore,
                        AudioDriver** out_driver) override;
  AudioDriver* CreateDriver(xe::threading::Semaphore* semaphore,
                            uint32_t frequency, uint32_t channels,
                            bool need_format_conversion) override;
  void DestroyDriver(AudioDriver* driver) override;

 protected:
  void Initialize() override;
};

}  // namespace phase
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_PHASE_PHASE_AUDIO_SYSTEM_H_
