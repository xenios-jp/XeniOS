/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_RELAY_SOCKET_H_
#define XENIA_NET_RELAY_SOCKET_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "xenia/net/relay_protocol.h"

namespace xe::net::relay {

// A UDP socket for System Link over the internet, IPv6 with IPv4 mapped where
// the host has it and IPv4 otherwise. One thread uses it, except for Wake().
class RelaySocket {
 public:
  RelaySocket();
  ~RelaySocket();

  // Binds |port| on every address. Port 0 takes any free one.
  bool Open(uint16_t port, std::string& error);
  uint16_t port() const;

  // False when the socket failed for good. Unreachable destinations aren't
  // failures.
  bool SendTo(std::span<const uint8_t> data, const Endpoint& to);

  enum class Wait { kDatagram, kTimeout, kWoken, kFailed };
  // Waits up to |timeout_ms| for one datagram, running the callbacks of
  // finished resolves meanwhile. |data| is valid until the next call.
  Wait ReceiveFor(uint32_t timeout_ms, std::span<const uint8_t>& data,
                  Endpoint& from, std::string& error);

  using Resolved =
      std::function<void(std::vector<Endpoint> endpoints, std::string error)>;
  // Looks up |host| in the background, with the addresses this socket can
  // send to first. |done| runs inside a later ReceiveFor.
  void Resolve(const std::string& host, uint16_t port, Resolved done);

  // Ends the ReceiveFor waiting on another thread, or the next one. Thread
  // safe.
  void Wake();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Splits "host", "host:port", "[v6]" or "[v6]:port", with |default_port|
// when there's no port.
bool ParseServer(std::string_view text, uint16_t default_port,
                 std::string& host, uint16_t& port);

}  // namespace xe::net::relay

#endif  // XENIA_NET_RELAY_SOCKET_H_
