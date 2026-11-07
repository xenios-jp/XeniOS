/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_VOICE_H_
#define XENIA_KERNEL_XAM_XAM_VOICE_H_

#include <cstdint>

#include "xenia/base/byte_order.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// The packet a title hands XamVoiceSubmitPacket, as Halo 3 fills it: one
// direction of one headset, 20 ms or less of audio.
struct X_VOICE_PACKET {
  // X_STATUS_PENDING while queued, 0 once the headset is done with it.
  xe::be<uint32_t> status;
  // Set by the headset on completion to the bytes it filled or consumed. The
  // title rewrites it between uses.
  xe::be<uint32_t> bytes_done;
  xe::be<uint32_t> buffer_ptr;
  // Bytes of headset data in the buffer: two 4-bit G.726 codes a byte.
  xe::be<uint32_t> buffer_size;
  xe::be<uint32_t> unknown_10;
  // 1 = 4-bit G.726, the only format seen: 16 kHz from the microphone, 8 kHz
  // to the earpiece.
  xe::be<uint32_t> format;
  // The title's own; the headset leaves it alone.
  xe::be<uint32_t> title_link;
};
static_assert(sizeof(X_VOICE_PACKET) == 0x1C, "X_VOICE_PACKET size");

constexpr uint32_t kVoicePacketFormatG726 = 1;
// XamVoiceSubmitPacket's direction argument.
constexpr uint32_t kVoicePacketPlayback = 0;
constexpr uint32_t kVoicePacketCapture = 1;

// Closes every headset and the host audio behind them. Before the kernel goes
// away.
void VoiceShutdown();

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_VOICE_H_
