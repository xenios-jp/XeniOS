/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_TESTING_FAKE_LOBBY_H_
#define XENIA_NET_TESTING_FAKE_LOBBY_H_

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "xenia/net/packet_transport.h"

namespace xe::net::testing {

// Stands in for the lobby while it lives: the lobby socket factory hands out
// sockets that keep what's sent, and the test calls their events.
class FakeLobby {
 public:
  FakeLobby() : old_(secure::GetLobbySocketFactory()) {
    secure::SetLobbySocketFactory(
        [this](const std::string& url, secure::LobbySocket::Events events) {
          {
            std::lock_guard lock(mutex_);
            urls_.push_back(url);
            events_ = std::move(events);
            ++sockets_;
          }
          changed_.notify_all();
          return std::make_unique<Socket>(this);
        });
  }
  ~FakeLobby() { secure::SetLobbySocketFactory(old_); }

  // The events of the |count|th socket opened, once it is.
  secure::LobbySocket::Events WaitForSocket(int count = 1) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return sockets_ >= count; });
    return events_;
  }
  // The URLs the client opened, in order.
  std::vector<std::string> Urls() {
    std::lock_guard lock(mutex_);
    return urls_;
  }
  // What the client sent so far.
  std::vector<std::string> Sent() {
    std::lock_guard lock(mutex_);
    return sent_;
  }

 private:
  struct Socket : secure::LobbySocket {
    explicit Socket(FakeLobby* lobby) : lobby(lobby) {}
    void Send(std::string text) override {
      std::lock_guard lock(lobby->mutex_);
      lobby->sent_.push_back(std::move(text));
    }
    FakeLobby* lobby;
  };

  secure::LobbySocketFactory old_;
  std::mutex mutex_;
  std::condition_variable changed_;
  secure::LobbySocket::Events events_;
  int sockets_ = 0;
  std::vector<std::string> sent_;
  std::vector<std::string> urls_;
};

}  // namespace xe::net::testing

#endif  // XENIA_NET_TESTING_FAKE_LOBBY_H_
