/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_APU_FLAGS_H_
#define XENIA_APU_APU_FLAGS_H_

#include "xenia/base/cvar.h"
DECLARE_uint32(volume)

    namespace xe {
  namespace apu {

  // Wraps OVERRIDE_PERSIST_uint32(volume, ...) so callers in other TUs can
  // write the config-value layer that SaveConfig() serializes.
  void SetVolumePersistent(uint32_t percent);

  // Applies volume for the session only (no config write); used for mute.
  void SetVolume(uint32_t percent);

  }  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_APU_FLAGS_H_
