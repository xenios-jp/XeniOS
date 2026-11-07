/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_live_social.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <sstream>
#include <thread>
#include <tuple>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_live_invites.h"
#include "xenia/kernel/xam/xam_live_party.h"
#include "xenia/kernel/xam/xam_live_sessions.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/kernel/xsession.h"
#include "xenia/net/live_account_hub.h"

DEFINE_string(live_friend_xuids, "",
              "Online XUIDs (hex, comma separated) the signed-in profiles ask "
              "to be friends with on the online service. Both sides asking "
              "makes them friends. For tests without the UI.",
              "Live");

namespace xe {
namespace kernel {
namespace xam {

namespace {

struct Slot {
  uint64_t offline_xuid = 0;
  uint32_t title_id = 0;  // The title friends see the profile in.
  uint32_t context = 0;
  // The Live session friends may join, as the title's sessions said.
  std::optional<net::live::Joinable> joinable;
  // The title's share of the profile's connection, which the app may share
  // too (it outlives the title then).
  std::shared_ptr<net::live::AccountShare> share;
  std::shared_ptr<net::live::AccountClient> account;
};

struct Social {
  std::mutex mutex;
  std::array<Slot, XUserMaxUserCount> slots;
  LiveInviteHandler invite_handler;
  // Threads destroying the accounts of profiles that signed out (see
  // StopAccounts), joined at shutdown.
  std::vector<std::thread> stoppers;
};

Social& GetSocial() {
  static Social social;
  return social;
}

// Destroying an account waits for its running event, which may wait for the
// kernel's global lock or Social::mutex, so a caller holding either hands the
// accounts to a thread of their own.
void StopAccounts(
    std::vector<std::shared_ptr<net::live::AccountShare>> accounts) {
  if (accounts.empty()) {
    return;
  }
  std::thread stopper([accounts = std::move(accounts)]() mutable {
    accounts.clear();
  });
  auto& social = GetSocial();
  std::lock_guard lock(social.mutex);
  social.stoppers.push_back(std::move(stopper));
}

std::vector<uint64_t> TestFriendXuids() {
  std::vector<uint64_t> xuids;
  std::stringstream list(cvars::live_friend_xuids);
  std::string item;
  while (std::getline(list, item, ',')) {
    if (!item.empty()) {
      xuids.push_back(std::strtoull(item.c_str(), nullptr, 16));
    }
  }
  return xuids;
}

std::shared_ptr<net::live::AccountShare> StartAccount(
    KernelState* kernel_state, uint32_t user_index, const std::string& lobby,
    uint64_t offline_xuid, uint64_t online_xuid, const std::string& gamertag) {
  // One key per device (per storage root), whichever profile signs in.
  const auto key_path =
      kernel_state->emulator()->storage_root() / "live" / "device_key";
  net::live::AccountClient::Events events;
  events.signed_in = [kernel_state, user_index, offline_xuid, online_xuid](
                         uint64_t xuid, const std::string& code) {
    XELOGI("Live account: user {} online as {:016X}, friend code {}",
           user_index, xuid, code);
    LiveSessionTokenReady();  // The session directory may be waiting.
    if (xuid == online_xuid) {
      return;
    }
    // The service gave the profile another online XUID (another device owns
    // this one): the profile keeps it from now on. Profiles change under the
    // kernel's global lock, and this runs on the account's socket thread.
    auto global_lock = xe::global_critical_region::AcquireDirect();
    auto* profiles = kernel_state->xam_state()->profile_manager();
    if (const auto* account = profiles->GetAccount(offline_xuid)) {
      X_XAMACCOUNTINFO updated = *account;
      updated.xuid_online = xuid;
      profiles->UpdateAccount(offline_xuid, &updated);
    }
  };
  events.friends_changed = [kernel_state, user_index] {
    XELOGI("Live account: user {} has {} friend(s)", user_index,
           LiveFriends(user_index).size());
    // Test friends whose request came first (the profile's own one found
    // nobody yet): asking back accepts it.
    if (auto account = LiveAccount(user_index)) {
      const auto test_friends = TestFriendXuids();
      for (const auto& peer : account->Friends()) {
        if (peer.state == net::live::Friend::State::kIncoming &&
            std::find(test_friends.begin(), test_friends.end(), peer.xuid) !=
                test_friends.end()) {
          account->AddFriend(peer.xuid);
        }
      }
    }
    kernel_state->BroadcastNotification(kXNotificationFriendsFriendAdded, 0);
    LiveInvitesPoll(user_index);
    LivePartyPoll(user_index);
  };
  events.blocked_changed = [kernel_state, user_index] {
    const auto account = LiveAccount(user_index);
    XELOGI("Live account: user {} blocks {} player(s)", user_index,
           account ? account->Blocked().size() : 0);
    kernel_state->BroadcastNotification(kXNotificationSystemMuteListChanged,
                                        0);
  };
  events.presence_changed = [kernel_state,
                             user_index](const net::live::Presence&) {
    kernel_state->BroadcastNotification(kXNotificationFriendsPresenceChanged,
                                        0);
    LivePartyPoll(user_index);
  };
  events.invited = [user_index](const net::live::Invite& invite) {
    LiveInviteHandler handler;
    {
      auto& social = GetSocial();
      std::lock_guard lock(social.mutex);
      handler = social.invite_handler;
    }
    if (handler) {
      handler(user_index, invite);
    } else {
      XELOGI("Live account: an invite came, but nothing takes invites");
    }
  };
  events.session_token = [](const std::string&, uint32_t) {
    LiveSessionTokenReady();
  };
  events.party_changed = [kernel_state, user_index] {
    kernel_state->BroadcastNotification(kXNotificationPartyMembersChanged, 0);
    LivePartyChanged(user_index);
  };
  auto share = net::live::ShareAccount(lobby, key_path, online_xuid, gamertag,
                                       std::move(events));
  if (!share) {
    return nullptr;
  }
  for (const uint64_t xuid : TestFriendXuids()) {
    if (xuid != online_xuid) {
      (*share)->AddFriend(xuid);
    }
  }
  return share;
}

// What friends may join: the title's Live session, in the System Link room
// the title plays in; else, in a System Link game (System Link through the
// relay, a key registered, and no session with Live presence), just that
// room, where the game's own System Link screen finds the game.
std::optional<net::live::Joinable> Joinable(const Slot& slot) {
  const auto room = SystemLinkRoomCode(slot.title_id);
  if (slot.joinable) {
    auto joinable = *slot.joinable;
    if (room && joinable.room.empty()) {
      joinable.room = *room;
    }
    return joinable;
  }
  if (room && SystemLinkKeysRegistered() && !LivePresenceSessionsOpen()) {
    net::live::Joinable joinable;
    joinable.room = *room;
    return joinable;
  }
  return std::nullopt;
}

// Friends see the profile online in the running title, until a session
// gives them something to join.
void ShowTitle(KernelState* kernel_state, Slot& slot) {
  const uint32_t title_id = kernel_state->title_id();
  if (slot.account && title_id != slot.title_id) {
    slot.title_id = title_id;
    slot.context = 0;
    slot.joinable.reset();
    slot.account->SetPresence(title_id, 0, {}, Joinable(slot));
  }
}

}  // namespace

void LiveSocialSignInChanged(KernelState* kernel_state, bool start_only) {
  auto* xam = kernel_state->xam_state();
  if (!xam) {
    // Profiles sign in while XAM is made; the title's start comes back here.
    return;
  }
  const std::string lobby = LiveSessionLobby();
  InstallLiveInvites(kernel_state);
  auto& social = GetSocial();
  std::vector<std::shared_ptr<net::live::AccountShare>> stopped;
  std::vector<uint32_t> signed_out;
  {
    std::lock_guard lock(social.mutex);
    for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
      auto& slot = social.slots[i];
      const auto* profile =
          xam->IsUserSignedIn(i) ? xam->GetUserProfile(i) : nullptr;
      const bool live =
          profile && !lobby.empty() &&
          profile->signin_state() ==
              static_cast<uint32_t>(SignInState::SignedInToLive);
      if (!live) {
        if (slot.share && !start_only) {
          stopped.push_back(std::move(slot.share));
          slot = {};
          signed_out.push_back(i);
        }
        continue;
      }
      if (slot.account &&
          (slot.offline_xuid == profile->xuid() || start_only)) {
        ShowTitle(kernel_state, slot);
        continue;
      }
      if (slot.share) {
        stopped.push_back(std::move(slot.share));
        signed_out.push_back(i);
      }
      slot.offline_xuid = profile->xuid();
      slot.title_id = 0;
      slot.share = StartAccount(kernel_state, i, lobby, profile->xuid(),
                                profile->GetOnlineXUID(), profile->name());
      slot.account = slot.share ? slot.share->client() : nullptr;
      ShowTitle(kernel_state, slot);
    }
  }
  // Another profile's invites aren't this one's.
  for (const uint32_t i : signed_out) {
    ForgetLiveInvites(i);
  }
  StopAccounts(std::move(stopped));
}

void LiveSocialShutdown() {
  auto& social = GetSocial();
  std::vector<std::shared_ptr<net::live::AccountShare>> shares;
  std::vector<std::thread> stoppers;
  {
    std::lock_guard lock(social.mutex);
    for (auto& slot : social.slots) {
      if (slot.share) {
        shares.push_back(std::move(slot.share));
      }
      slot = {};
    }
    stoppers = std::move(social.stoppers);
    social.stoppers.clear();
  }
  ShutdownLiveInvites();
  // Waits for the shares' running events, which may still use the kernel.
  shares.clear();
  for (auto& stopper : stoppers) {
    stopper.join();
  }
}

std::shared_ptr<net::live::AccountClient> LiveAccount(uint32_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return nullptr;
  }
  auto& social = GetSocial();
  std::lock_guard lock(social.mutex);
  return social.slots[user_index].account;
}

std::vector<net::live::Friend> LiveFriends(uint32_t user_index) {
  std::vector<net::live::Friend> friends;
  if (auto account = LiveAccount(user_index)) {
    for (auto& f : account->Friends()) {
      if (f.state == net::live::Friend::State::kFriend) {
        friends.push_back(std::move(f));
      }
    }
  }
  return friends;
}

bool LiveAreFriends(uint32_t user_index, uint64_t xuid) {
  auto account = LiveAccount(user_index);
  return account && account->IsFriend(xuid);
}

bool LiveIsBlocked(uint32_t user_index, uint64_t xuid) {
  auto account = LiveAccount(user_index);
  return account && account->IsBlocked(xuid);
}

std::optional<net::live::Presence> LiveFriendPresence(uint32_t user_index,
                                                      uint64_t xuid) {
  auto account = LiveAccount(user_index);
  return account ? account->FriendPresence(xuid) : std::nullopt;
}

void LiveSetPresence(uint32_t user_index, uint32_t title_id, uint32_t context,
                     std::optional<net::live::Joinable> joinable) {
  std::shared_ptr<net::live::AccountClient> account;
  {
    auto& social = GetSocial();
    std::lock_guard lock(social.mutex);
    if (user_index >= XUserMaxUserCount) {
      return;
    }
    auto& slot = social.slots[user_index];
    account = slot.account;
    slot.title_id = title_id;
    slot.context = context;
    slot.joinable = std::move(joinable);
    joinable = Joinable(slot);
  }
  if (account) {
    account->SetPresence(title_id, context, {}, std::move(joinable));
    LiveInvitesPoll(user_index);
  }
}

void LiveSystemLinkChanged() {
  std::vector<std::tuple<uint32_t, std::shared_ptr<net::live::AccountClient>,
                         uint32_t, uint32_t,
                         std::optional<net::live::Joinable>>>
      updates;
  {
    auto& social = GetSocial();
    std::lock_guard lock(social.mutex);
    for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
      const auto& slot = social.slots[i];
      if (slot.account && slot.title_id) {
        updates.emplace_back(i, slot.account, slot.title_id, slot.context,
                             Joinable(slot));
      }
    }
  }
  // The account sends it only when it changed.
  for (auto& [user_index, account, title_id, context, joinable] : updates) {
    account->SetPresence(title_id, context, {}, std::move(joinable));
    LiveInvitesPoll(user_index);
  }
}

std::optional<net::live::Joinable> LiveOwnJoinable(uint32_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return std::nullopt;
  }
  auto& social = GetSocial();
  std::lock_guard lock(social.mutex);
  return Joinable(social.slots[user_index]);
}

void SetLiveInviteHandler(LiveInviteHandler handler) {
  auto& social = GetSocial();
  std::lock_guard lock(social.mutex);
  social.invite_handler = std::move(handler);
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
