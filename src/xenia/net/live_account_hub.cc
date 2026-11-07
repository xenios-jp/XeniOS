/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/live_account_hub.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"

namespace xe::net::live {

// One connection and the events of every share of it. The connection's events
// hold the entry, so it lives as long as the connection.
struct AccountShare::Entry {
  std::string lobby;
  uint64_t xuid = 0;
  std::weak_ptr<AccountClient> client;

  std::mutex mutex;
  std::condition_variable idle;
  std::vector<std::pair<uint64_t, AccountClient::Events>> listeners;
  uint64_t next_listener = 1;
  // The threads calling listeners now.
  std::vector<std::thread::id> dispatching;

  template <typename Call>
  void Dispatch(Call&& call) {
    std::vector<AccountClient::Events> targets;
    {
      std::lock_guard lock(mutex);
      targets.reserve(listeners.size());
      for (const auto& [id, events] : listeners) {
        targets.push_back(events);
      }
      dispatching.push_back(std::this_thread::get_id());
    }
    for (const auto& events : targets) {
      call(events);
    }
    {
      std::lock_guard lock(mutex);
      auto it = std::find(dispatching.begin(), dispatching.end(),
                          std::this_thread::get_id());
      if (it != dispatching.end()) {
        dispatching.erase(it);
      }
    }
    idle.notify_all();
  }

  uint64_t Add(AccountClient::Events events) {
    std::lock_guard lock(mutex);
    const uint64_t id = next_listener++;
    listeners.emplace_back(id, std::move(events));
    return id;
  }

  // Waits for calls on other threads, which may still have the listener.
  void Remove(uint64_t id) {
    std::unique_lock lock(mutex);
    std::erase_if(listeners, [id](const auto& l) { return l.first == id; });
    const auto self = std::this_thread::get_id();
    idle.wait(lock, [&] {
      return std::all_of(dispatching.begin(), dispatching.end(),
                         [&](std::thread::id t) { return t == self; });
    });
  }
};

namespace {

std::mutex registry_mutex;
std::vector<std::weak_ptr<AccountShare::Entry>>& Registry() {
  static std::vector<std::weak_ptr<AccountShare::Entry>> registry;
  return registry;
}

}  // namespace

AccountShare::AccountShare(std::shared_ptr<Entry> entry,
                           std::shared_ptr<AccountClient> client,
                           uint64_t listener)
    : entry_(std::move(entry)),
      client_(std::move(client)),
      listener_(listener) {}

AccountShare::~AccountShare() {
  entry_->Remove(listener_);
  // The last share closes the connection (and waits for its thread).
  client_.reset();
}

namespace {
// Guarded by registry_mutex.
std::function<bool(uint64_t)>& AppearOfflineProvider() {
  static std::function<bool(uint64_t)> provider;
  return provider;
}
}  // namespace

void SetAppearOfflineProvider(
    std::function<bool(uint64_t online_xuid)> provider) {
  std::lock_guard registry_lock(registry_mutex);
  AppearOfflineProvider() = std::move(provider);
}

std::shared_ptr<AccountShare> ShareAccount(const std::string& lobby,
                                           const std::filesystem::path& key_path,
                                           uint64_t online_xuid,
                                           const std::string& gamertag,
                                           AccountClient::Events events) {
  using Entry = AccountShare::Entry;
  std::lock_guard registry_lock(registry_mutex);
  auto& registry = Registry();
  std::erase_if(registry, [](const auto& e) { return e.expired(); });
  for (const auto& weak : registry) {
    auto entry = weak.lock();
    if (!entry || entry->lobby != lobby) {
      continue;
    }
    auto client = entry->client.lock();
    // A stopped client (signed in elsewhere, say) is left to its holders: a
    // new sign-in gets a client of its own. The lobby may have moved the
    // account to another XUID since.
    if (client && !client->halted() &&
        (entry->xuid == online_xuid ||
                   (online_xuid && client->xuid() == online_xuid))) {
      const uint64_t id = entry->Add(std::move(events));
      XELOGI("Live account: sharing the open connection for {:016X}",
             client->xuid());
      return std::shared_ptr<AccountShare>(
          new AccountShare(std::move(entry), std::move(client), id));
    }
  }

  auto key = DeviceKey::LoadOrCreate(key_path);
  if (!key) {
    XELOGE("Live account: no device key, so no friends or invites");
    return nullptr;
  }
  auto entry = std::make_shared<Entry>();
  entry->lobby = lobby;
  entry->xuid = online_xuid;
  const uint64_t id = entry->Add(std::move(events));

  // Every event goes to every share.
  AccountClient::Events fan_out;
  auto forward = [&entry](auto member) {
    return [entry, member](const auto&... args) {
      entry->Dispatch([&](const AccountClient::Events& e) {
        if (e.*member) (e.*member)(args...);
      });
    };
  };
  using E = AccountClient::Events;
  fan_out.signed_in = forward(&E::signed_in);
  fan_out.friends_changed = forward(&E::friends_changed);
  fan_out.blocked_changed = forward(&E::blocked_changed);
  fan_out.presence_changed = forward(&E::presence_changed);
  fan_out.invited = forward(&E::invited);
  fan_out.party_changed = forward(&E::party_changed);
  fan_out.name_checked = forward(&E::name_checked);
  fan_out.session_token = forward(&E::session_token);
  fan_out.recovery_code = forward(&E::recovery_code);
  fan_out.link_code = forward(&E::link_code);
  fan_out.join_pending = forward(&E::join_pending);
  fan_out.join_requested = forward(&E::join_requested);
  fan_out.join_closed = forward(&E::join_closed);
  fan_out.devices_changed = forward(&E::devices_changed);
  fan_out.profile_changed = forward(&E::profile_changed);
  fan_out.rename_failed = forward(&E::rename_failed);
  fan_out.request_failed = forward(&E::request_failed);
  fan_out.moved = forward(&E::moved);
  fan_out.join_failed = forward(&E::join_failed);
  fan_out.register_refused = forward(&E::register_refused);
  fan_out.removed = forward(&E::removed);
  fan_out.signed_in_elsewhere = forward(&E::signed_in_elsewhere);
  fan_out.invite_answered = forward(&E::invite_answered);
  fan_out.invite_state = forward(&E::invite_state);
  fan_out.connection_changed = forward(&E::connection_changed);
  fan_out.reported = forward(&E::reported);
  fan_out.report_failed = forward(&E::report_failed);
  fan_out.names = forward(&E::names);
  fan_out.settings_changed = forward(&E::settings_changed);
  fan_out.friend_add_refused = forward(&E::friend_add_refused);
  fan_out.search_results = forward(&E::search_results);
  AccountOptions options;
  if (const auto& appear_offline = AppearOfflineProvider()) {
    options.appear_offline = appear_offline(online_xuid);
  }
  auto client = std::make_shared<AccountClient>(
      lobby, std::move(*key), online_xuid, gamertag, std::move(fan_out),
      std::move(options));
  entry->client = client;
  registry.push_back(entry);
  return std::shared_ptr<AccountShare>(
      new AccountShare(std::move(entry), std::move(client), id));
}

void ForgetAccount(const std::string& lobby, uint64_t online_xuid) {
  std::lock_guard registry_lock(registry_mutex);
  std::erase_if(Registry(), [&](const auto& weak) {
    auto entry = weak.lock();
    if (!entry) {
      return true;
    }
    if (entry->lobby != lobby) {
      return false;
    }
    auto client = entry->client.lock();
    return entry->xuid == online_xuid ||
           (client && client->xuid() == online_xuid);
  });
}

}  // namespace xe::net::live
