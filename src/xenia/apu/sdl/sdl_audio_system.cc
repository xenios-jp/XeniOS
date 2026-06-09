/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sdl/sdl_audio_system.h"

#include <atomic>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/sdl/sdl_audio_driver.h"
#include "xenia/apu/silent_audio_driver.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
namespace sdl {

std::unique_ptr<AudioSystem> SDLAudioSystem::Create(cpu::Processor* processor) {
  return std::make_unique<SDLAudioSystem>(processor);
}

SDLAudioSystem::SDLAudioSystem(cpu::Processor* processor)
    : AudioSystem(processor) {}

SDLAudioSystem::~SDLAudioSystem() {}

void SDLAudioSystem::Initialize() { AudioSystem::Initialize(); }

X_STATUS SDLAudioSystem::CreateDriver(size_t index,
                                      xe::threading::Semaphore* semaphore,
                                      AudioDriver** out_driver) {
  assert_not_null(out_driver);
#if XE_PLATFORM_IOS
  // iOS/SDL audio path is single-output in practice here. Additional guest
  // clients should not attempt to open another device because SDL reports
  // "Audio device already open" and we fall back to silence anyway.
  if (index > 0) {
    static std::atomic<bool> logged_secondary_fallback{false};
    if (!logged_secondary_fallback.exchange(true, std::memory_order_relaxed)) {
      XELOGW(
          "SDLAudioSystem: iOS secondary audio clients use silent fallback; "
          "keeping primary SDL device on client 0");
    }
    *out_driver = new SilentAudioDriver(semaphore);
    return X_STATUS_SUCCESS;
  }
#endif

  auto driver = std::make_unique<SDLAudioDriver>(semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
#if XE_PLATFORM_IOS
    XELOGW("SDLAudioSystem: SDL audio init failed, using silent fallback");
    *out_driver = new SilentAudioDriver(semaphore);
    return X_STATUS_SUCCESS;
#else
    return X_STATUS_UNSUCCESSFUL;
#endif
  }

  *out_driver = driver.release();
  return X_STATUS_SUCCESS;
}

AudioDriver* SDLAudioSystem::CreateDriver(xe::threading::Semaphore* semaphore,
                                          uint32_t frequency, uint32_t channels,
                                          bool need_format_conversion) {
#if XE_PLATFORM_IOS
  static std::atomic<bool> logged_independent_fallback{false};
  if (!logged_independent_fallback.exchange(true, std::memory_order_relaxed)) {
    XELOGW(
        "SDLAudioSystem: iOS independent audio drivers use silent fallback; "
        "secondary SDL/CoreAudio devices are not opened");
  }
  return new SilentAudioDriver(semaphore);
#else
  return new SDLAudioDriver(semaphore, frequency, channels,
                            need_format_conversion);
#endif  // XE_PLATFORM_IOS
}

void SDLAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
#if XE_PLATFORM_IOS
  if (auto* silent_driver = dynamic_cast<SilentAudioDriver*>(driver)) {
    silent_driver->Shutdown();
    delete silent_driver;
    return;
  }
#endif
  auto sdldriver = dynamic_cast<SDLAudioDriver*>(driver);
  assert_not_null(sdldriver);
  sdldriver->Shutdown();
  delete sdldriver;
}

}  // namespace sdl
}  // namespace apu
}  // namespace xe
