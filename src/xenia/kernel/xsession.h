/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XSESSION_H_
#define XENIA_KERNEL_XSESSION_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "xenia/kernel/xobject.h"
#include "xenia/net/xnet_secure.h"

namespace xe {
namespace net {
class SystemLink;
}  // namespace net

namespace kernel {

enum SessionFlags {
  HOST = 0x01,
  PRESENCE = 0x02,
  STATS = 0x04,
  MATCHMAKING = 0x08,
  ARBITRATION = 0x10,
  PEER_NETWORK = 0x20,
  SOCIAL_MATCHMAKING_ALLOWED = 0x80,
  INVITES_DISABLED = 0x0100,
  JOIN_VIA_PRESENCE_DISABLED = 0x0200,
  JOIN_IN_PROGRESS_DISABLED = 0x0400,
  JOIN_VIA_PRESENCE_FRIENDS_ONLY = 0x0800,
  UNKNOWN = 0x1000,  // 4156091D and 5841128F sets this flag?

  SINGLEPLAYER_WITH_STATS = PRESENCE | STATS | INVITES_DISABLED |
                            JOIN_VIA_PRESENCE_DISABLED |
                            JOIN_IN_PROGRESS_DISABLED,

  LIVE_MULTIPLAYER_STANDARD = PRESENCE | STATS | MATCHMAKING | PEER_NETWORK,
  LIVE_MULTIPLAYER_RANKED = LIVE_MULTIPLAYER_STANDARD | ARBITRATION,
  SYSTEMLINK = PEER_NETWORK,
  GROUP_LOBBY = PRESENCE | PEER_NETWORK,
  GROUP_GAME = STATS | MATCHMAKING | PEER_NETWORK,

  // HELPERS
  SYSTEMLINK_FEATURES = HOST | SYSTEMLINK,
};

struct XSESSION_INFO {
  net::KeyId session_id;
  net::secure::Address host_address;
  net::Key key_exchange_key;
};
static_assert_size(XSESSION_INFO, 0x3C);

class XSession : public XObject {
 public:
  static const Type kObjectType = Type::Session;

  explicit XSession(KernelState* kernel_state);
  ~XSession() override;

  X_STATUS Initialize();

  // The session whose guest body is at |guest_ptr|.
  static object_ref<XSession> FromGuest(KernelState* kernel_state,
                                        uint32_t guest_ptr);

  // A host gets a new key and its own XNADDR in the session info. A peer
  // network session keeps the key registered until DeleteSession, even after
  // its handle closes.
  X_RESULT Create(uint32_t flags, uint32_t user_index,
                  uint32_t max_public_slots, uint32_t max_private_slots,
                  uint32_t session_info_ptr, uint32_t nonce_ptr);
  X_RESULT DeleteSession();
  // Local users join and leave by index, remote ones by XUID.
  void Join(uint32_t count, uint32_t xuids_ptr, uint32_t user_indices_ptr,
            uint32_t private_slots_ptr);
  void Leave(uint32_t count, uint32_t xuids_ptr, uint32_t user_indices_ptr);
  void Start();
  void End();
  X_RESULT Modify(uint32_t flags, uint32_t max_public_slots,
                  uint32_t max_private_slots);
  // XSessionArbitrationRegister: the machines and players of a ranked session,
  // the same list on every machine in it.
  X_RESULT RegisterArbitration(uint32_t results_size, uint32_t results_ptr);
  // Fills XSESSION_LOCAL_DETAILS and as many members as fit in the buffer.
  X_RESULT GetDetails(uint32_t size_ptr, uint32_t details_ptr);

 private:
  enum State : uint32_t {
    kLobby = 0,
    kRegistration = 1,
    kInGame = 2,
    kReporting = 3,
    kDeleted = 4,
  };

  struct Member {
    uint64_t xuid = 0;
    uint32_t user_index = 0;
    bool private_slot = false;
  };

  // A context of the user who created the session, 0 when unset.
  uint32_t CreatorContext(uint32_t id) const;
  // Public and private slots the members take, with mutex_ held.
  std::pair<uint32_t, uint32_t> UsedSlots() const;
  // Announces a hosted matchmaking session to the title's other players, or
  // withdraws it once nobody can join, with mutex_ held.
  void PublishLocked();
  // After a join or leave: just the slots when the lobby lists the session,
  // with mutex_ held.
  void PublishSlotsLocked();
  // Tells the creator's friends about the session (Live presence), so they
  // can join it, with mutex_ held.
  void UpdatePresenceLocked();
  // Tells SessionRemotePlayers who else is in this session, with mutex_ held.
  void PublishRemotePlayersLocked();
  // Counts it among LivePresenceSessionsOpen, or stops counting it.
  void CountPresence(bool open);

  std::weak_ptr<net::SystemLink> system_link_;
  std::optional<net::KeyId> registered_key_;

  // Requests for one session can arrive on several threads.
  std::mutex mutex_;
  uint32_t flags_ = 0;
  uint32_t user_index_ = XUserIndexNone;
  uint32_t max_public_slots_ = 0;
  uint32_t max_private_slots_ = 0;
  uint32_t game_type_ = 0;
  uint32_t game_mode_ = 0;
  State state_ = kLobby;
  uint64_t nonce_ = 0;
  XSESSION_INFO info_{};
  bool has_info_ = false;
  net::KeyId arbitration_id_{};
  bool published_ = false;
  bool in_presence_ = false;
  bool presence_counted_ = false;
  // Its key counts toward SystemLinkKeysRegistered.
  bool key_counted_ = false;
  std::vector<Member> members_;
};

// How many sessions with Live presence the running title has open. A title
// playing System Link through the relay with none is in a System Link game
// (titles host those without a session, or with one without presence).
uint32_t LivePresenceSessionsOpen();

// The players the running title put in its sessions who aren't signed in on this
// console (XSessionJoinRemote), while those sessions last, each once. For the
// app's list of the players in the game.
std::vector<uint64_t> SessionRemotePlayers();
// |changed| is called, on whichever thread changed it, when that list may have
// changed. One listener; an empty function removes it. It runs with that
// session's lock held (not the registry's), so it must only note the change and
// return: it must not block or call back into the session.
void SetSessionRemotePlayersListener(std::function<void()> changed);

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XSESSION_H_
