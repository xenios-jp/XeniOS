/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/apu_flags.h"

DEFINE_uint32(volume, 100,
              "Master volume for all audio output, from 0 (silent) to 100 "
              "(full volume).",
              "APU")

    namespace xe {
  namespace apu {

  void SetVolumePersistent(uint32_t percent) {
    OVERRIDE_PERSIST_uint32(volume, percent);
  }

  void SetVolume(uint32_t percent) { OVERRIDE_uint32(volume, percent); }

  }  // namespace apu
}  // namespace xe
