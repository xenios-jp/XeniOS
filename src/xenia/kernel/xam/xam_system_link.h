/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_SYSTEM_LINK_H_
#define XENIA_KERNEL_XAM_XAM_SYSTEM_LINK_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "xenia/kernel/xobject.h"
#include "xenia/net/system_link.h"

namespace xe {
namespace kernel {

class KernelState;
class XHostThread;

namespace xam {

// Whether System Link is on, which is also whether the console has a network
// link.
bool SystemLinkEnabled();

// Lets an app keep the relay behind its own consent and pick each title's
// room. |allowed| false turns System Link off while the transport is the
// relay. It is read when the policy is set and when SystemLinkAllowedChanged
// is called, not on every use. |room| gives a private room's code for a title,
// or empty for its public lobby, in place of systemlink_room. Both are
// optional and are called on emulator threads.
struct SystemLinkRelayPolicy {
  std::function<bool()> allowed;
  std::function<std::string(uint32_t title_id)> room;
  // The app's ID for counting open apps, which the room counts while the
  // title plays in it (see relay::RelayConfig::app_id). Optional.
  std::function<std::string()> app_id;
  // A title asked whether the console has a network link while System Link
  // is off, the first time in this run for each title. Optional.
  std::function<void(uint32_t title_id)> link_off;
  // Makes |code| (empty: the public lobby) |title_id|'s room for this launch,
  // to play with a friend there (JoinSystemLinkRoom). Optional: without it
  // systemlink_room is set.
  std::function<void(uint32_t title_id, const std::string& code)> join_room;
};
void SetSystemLinkRelayPolicy(SystemLinkRelayPolicy policy);
// What the policy's |allowed| or systemlink_transport says may have changed.
// When System Link turned on or off, running games are told the network link
// changed.
void SystemLinkAllowedChanged();
// The policy's room for a title may have changed. A running title moves to it
// within about a second, leaving the players in its old room.
void SystemLinkRoomChanged();
// The room |title_id| plays System Link in through the relay: a private
// room's code, or empty for its public lobby. Nothing when System Link is off
// or doesn't go through the relay, or that title hasn't started it.
std::optional<std::string> SystemLinkRoomCode(uint32_t title_id);
// The running title registered (or unregistered) an XNet key with System
// Link: its own (XNetRegisterKey) or a session's.
void SystemLinkKeyRegistered(bool registered);
// Whether it has one: it hosts or plays a networked game.
bool SystemLinkKeysRegistered();
// Moves |title_id| into |code|'s room (empty: its public lobby) for this
// launch, to play with a friend there; a running title moves within about a
// second.
void JoinSystemLinkRoom(uint32_t title_id, const std::string& code);
// The running title asked for the network link while System Link is off.
void SystemLinkOffQueried(KernelState* kernel_state);

// The lobby whose session directory Live matchmaking uses: the relay's, when
// System Link goes through the relay. Empty otherwise.
std::string LiveSessionLobby();

// Whether the running title has cross-platform System Link.
bool CrossPlatformTitle(KernelState* kernel_state);
// Whether the running title uses cross-platform System Link.
bool CrossPlatformSystemLink(KernelState* kernel_state);

// Starts System Link for the running title over the configured transport, on
// UDP |port| for cross-platform System Link. Null when it is off or cannot
// start.
std::unique_ptr<net::SystemLink> CreateSystemLink(KernelState* kernel_state,
                                                  uint16_t port);

// The running title's System Link, started on first use. Null when it is off
// or could not start, then the title gets no System Link at all.
std::shared_ptr<net::SystemLink> GetSystemLink(KernelState* kernel_state);

// Hands a title with its own network stack, such as the original Xbox
// emulator, the raw Ethernet frames that arrive on the System Link LAN.
class EthernetIntercept {
 public:
  explicit EthernetIntercept(KernelState* kernel_state);
  ~EthernetIntercept();

  // XnpEthernetInterceptSetCallbacks: |receive| gets |context| and each frame
  // that arrives on |link|, at DISPATCH_LEVEL as xam calls it. 0 removes the
  // callback of |caller|. Returns a Winsock error.
  uint32_t SetCallbacks(net::SystemLink& link, uint32_t caller,
                        uint32_t receive, uint32_t context);

 private:
  struct Queue;

  bool Start();
  // |queue| is a reference since a fiber's stack is freed without unwinding.
  static void Deliver(KernelState* kernel_state, Queue& queue, uint32_t buffer);

  KernelState* kernel_state_;
  std::shared_ptr<Queue> queue_;
  object_ref<XHostThread> thread_;
  // Guest memory the frame is in while |receive| runs.
  uint32_t buffer_ = 0;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_SYSTEM_LINK_H_
