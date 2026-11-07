/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_LIVE_SESSIONS_H_
#define XENIA_KERNEL_XAM_XAM_LIVE_SESSIONS_H_

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "xenia/kernel/xsession.h"

namespace xe {
namespace kernel {
class KernelState;

namespace xam {

// Live matchmaking without a Live session service. On a LAN a host announces
// each matchmaking session over System Link and a search collects the
// announcements. Through the relay, a host lists the session in the lobby's
// session directory instead, and a search asks the directory. Players stay in
// the title's public System Link lobby, where joining (and the title's QoS
// probe of the host before it) goes over System Link as on a LAN.
struct LiveSessionAdvert {
  XSESSION_INFO info{};
  uint32_t flags = 0;
  uint32_t max_public_slots = 0;
  uint32_t max_private_slots = 0;
  uint32_t used_public_slots = 0;
  uint32_t used_private_slots = 0;
  uint64_t host_xuid = 0;
  bool in_game = false;
  struct Context {
    uint32_t id;
    uint32_t value;
  };
  // Scalar properties only, as an X_USER_DATA in guest layout.
  struct Property {
    uint32_t id;
    std::array<uint8_t, 16> data;
  };
  std::vector<Context> contexts;
  std::vector<Property> properties;
};

// Announces |advert|, replacing an earlier one with the same session ID.
void AdvertiseLiveSession(KernelState* kernel_state,
                          const LiveSessionAdvert& advert);
void WithdrawLiveSession(const net::KeyId& session_id);

// The sessions other players of the running title announce. Asks them to
// announce again and waits up to |wait_ms| when none were heard yet.
std::vector<LiveSessionAdvert> FindLiveSessions(KernelState* kernel_state,
                                                uint32_t wait_ms);

// XSessionArbitrationRegister without Live: each machine in a ranked session
// broadcasts its players for the session, and every machine ends up with the
// same list. Waits up to |wait_ms| for registrations that cover |members|,
// then returns all heard, this one included, sorted by machine ID.
struct ArbitrationRegistrant {
  uint64_t machine_id = 0;
  std::vector<uint64_t> xuids;
};
std::vector<ArbitrationRegistrant> RegisterArbitration(
    KernelState* kernel_state, const net::KeyId& session_id,
    const ArbitrationRegistrant& self, const std::vector<uint64_t>& members,
    uint32_t wait_ms);

// The announcement's wire format (xam_live_sessions_test.cc).
std::vector<uint8_t> EncodeLiveSessionAdvert(uint32_t title_id,
                                             const LiveSessionAdvert& advert);
bool DecodeLiveSessionAdvert(std::span<const uint8_t> bytes,
                             uint32_t& title_id, LiveSessionAdvert& advert);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_LIVE_SESSIONS_H_
