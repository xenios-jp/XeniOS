/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_DEVICE_SIGNAL_APPLE_H_
#define XENIA_NET_DEVICE_SIGNAL_APPLE_H_

#include <TargetConditionals.h>

#include <string>

namespace xe::net {

// What the lobby bans a device by when its key is gone: a hash, as 64 hex
// digits, of a random secret kept in the Keychain, which outlasts deleting the
// app. The secret never leaves this device. Empty if the Keychain can't be read
// right now, in which case nothing is created and the next call tries again.
std::string KeychainDeviceSignal();

#if TARGET_OS_OSX
// The same for a Mac, from a hash of its hardware UUID, which outlasts deleting
// the app and reinstalling macOS. Empty if it can't be read.
std::string MachineDeviceSignal();
#endif

}  // namespace xe::net

#endif  // XENIA_NET_DEVICE_SIGNAL_APPLE_H_
