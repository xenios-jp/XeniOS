/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/phase/phase_audio_system.h"

#include "xenia/apu/phase/phase_audio_driver.h"
#include "xenia/apu/silent_audio_driver.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
namespace phase {

std::unique_ptr<AudioSystem> PHASEAudioSystem::Create(
    cpu::Processor* processor) {
  return std::make_unique<PHASEAudioSystem>(processor);
}

PHASEAudioSystem::PHASEAudioSystem(cpu::Processor* processor)
    : AudioSystem(processor) {}

PHASEAudioSystem::~PHASEAudioSystem() {}

void PHASEAudioSystem::Initialize() { AudioSystem::Initialize(); }

X_STATUS PHASEAudioSystem::CreateDriver(size_t index,
                                        xe::threading::Semaphore* semaphore,
                                        AudioDriver** out_driver) {
  assert_not_null(out_driver);
  // Only guest client 0 drives the single shared PHASE engine. Additional
  // clients use a silent fallback: there is one physical output, and standing
  // up multiple engines would contend and waste CPU on a mobile device.
  if (index > 0) {
    *out_driver = new SilentAudioDriver(semaphore);
    return X_STATUS_SUCCESS;
  }

  auto driver = std::make_unique<PHASEAudioDriver>(semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    XELOGW("PHASEAudioSystem: PHASE init failed, using silent fallback");
    *out_driver = new SilentAudioDriver(semaphore);
    return X_STATUS_SUCCESS;
  }

  *out_driver = driver.release();
  return X_STATUS_SUCCESS;
}

AudioDriver* PHASEAudioSystem::CreateDriver(xe::threading::Semaphore* semaphore,
                                            uint32_t frequency,
                                            uint32_t channels,
                                            bool need_format_conversion) {
  (void)frequency;
  (void)channels;
  (void)need_format_conversion;
  // The independent/XMP path carries stereo content we don't spatialize; keep
  // it silent under the PHASE backend for now.
  return new SilentAudioDriver(semaphore);
}

void PHASEAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  // AudioDriver is polymorphic (virtual Shutdown + dtor), so this handles both
  // PHASEAudioDriver and SilentAudioDriver without a downcast.
  driver->Shutdown();
  delete driver;
}

}  // namespace phase
}  // namespace apu
}  // namespace xe
