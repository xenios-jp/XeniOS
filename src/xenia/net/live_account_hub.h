/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_LIVE_ACCOUNT_HUB_H_
#define XENIA_NET_LIVE_ACCOUNT_HUB_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "xenia/net/live_account.h"

namespace xe::net::live {

// One account connection per profile for the whole process. The app (its
// friends screens, home-screen invites) and the running title (XAM) both hold
// a share of it, so starting a game doesn't sign the profile in a second time
// and the connection outlives the game. Each share has its own events; the
// connection closes when the last share goes.
class AccountShare {
 public:
  struct Entry;  // Private to the hub.
  ~AccountShare();
  AccountShare(const AccountShare&) = delete;
  AccountShare& operator=(const AccountShare&) = delete;

  const std::shared_ptr<AccountClient>& client() const { return client_; }
  AccountClient* operator->() const { return client_.get(); }

 private:
  friend std::shared_ptr<AccountShare> ShareAccount(
      const std::string&, const std::filesystem::path&, uint64_t,
      const std::string&, AccountClient::Events);
  AccountShare(std::shared_ptr<Entry> entry,
               std::shared_ptr<AccountClient> client, uint64_t listener);

  std::shared_ptr<Entry> entry_;
  std::shared_ptr<AccountClient> client_;
  uint64_t listener_;
};

// The account of |online_xuid| on |lobby|, connected with the device key at
// |key_path| (made when missing). Joins the connection that's open for that
// account already, or opens one. |events| are this share's.
// Destroying the share waits for its events in flight (unless one of them is
// destroying it). A share that joins a connection that's signed in already
// gets no signed_in: check client()->signed_in(). Nothing when there's no
// device key.
std::shared_ptr<AccountShare> ShareAccount(const std::string& lobby,
                                           const std::filesystem::path& key_path,
                                           uint64_t online_xuid,
                                           const std::string& gamertag,
                                           AccountClient::Events events);

// Stops sharing the connections open for |online_xuid| on |lobby|, so the next
// ShareAccount for it opens a new one. For when this device has just joined
// that account: a connection opened before the join couldn't sign in to it.
// Shares made already keep their connection.
void ForgetAccount(const std::string& lobby, uint64_t online_xuid);

// Asked, when ShareAccount opens a connection for |online_xuid|, whether it
// starts appearing offline: the app keeps that setting, and a title may open
// the connection before the app does. Called with the hub's lock held: it must
// not share or forget an account. One provider; empty removes it.
void SetAppearOfflineProvider(
    std::function<bool(uint64_t online_xuid)> provider);

}  // namespace xe::net::live

#endif  // XENIA_NET_LIVE_ACCOUNT_HUB_H_
