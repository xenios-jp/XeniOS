/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_PACKET_TRANSPORT_H_
#define XENIA_NET_PACKET_TRANSPORT_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "xenia/net/host_interfaces.h"
#include "xenia/net/relay_client.h"
#include "xenia/net/xnet_secure.h"

namespace xe::net::secure {

// Carries this console's Ethernet frames to and from the System Link LAN.
class PacketTransport {
 public:
  virtual ~PacketTransport() = default;
  virtual Mac identity() const = 0;
  virtual uint32_t mtu() const = 0;
  virtual bool healthy() const { return true; }
  // Returns the bytes sent or -1.
  virtual int Send(std::span<const uint8_t> frame) = 0;
  // False on timeout, error or Wake().
  virtual bool Receive(std::vector<uint8_t>& frame, uint32_t timeout_ms) = 0;
  // Ends the Receive waiting on another thread, or the next one if none is.
  // Thread safe.
  virtual void Wake() = 0;
  // Moves a transport on the host network to another UDP port.
  virtual bool SetPort(uint16_t port) { return false; }
  // The title started System Link play. A transport that reaches strangers
  // stays quiet until then. Thread safe, and cheap to call again.
  virtual void Activate() {}
};

// Host byte order.
struct UdpEndpoint {
  uint32_t address = 0;
  uint16_t port = 0;
};

inline constexpr UdpEndpoint kKaiEngine{0x7F000001, 34523};

// Exchanges frames with an XLink Kai engine over its DDS protocol. Kai drops
// frames whose source MAC is not from a console vendor.
std::unique_ptr<PacketTransport> OpenKaiTransport(const Mac& mac,
                                                  UdpEndpoint engine,
                                                  std::string& error);

// Sends every frame as a UDP datagram to |destinations| and receives on
// |port|, which several instances on one machine can share. Reaches other
// Xenia instances only.
std::unique_ptr<PacketTransport> OpenUdpTunnelTransport(
    const Mac& mac, uint16_t port, std::vector<UdpEndpoint> destinations,
    std::string& error);

// Sends the datagram of each cross-platform frame on the host network from
// |host|'s address, on UDP |port| both ways, and receives the datagrams that
// arrive there as frames.
std::unique_ptr<PacketTransport> OpenHostTransport(const HostInterface& host,
                                                   uint16_t port,
                                                   std::string& error);

// A WebSocket to the System Link lobby, which the platform opens. Its events
// may come on any thread until it's destroyed.
class LobbySocket {
 public:
  struct Events {
    std::function<void()> opened;
    std::function<void(std::string text)> message;
    // Also when it couldn't connect. Nothing follows it. When the server
    // answered with an HTTP error instead of the connection, |reason| is
    // "http <status>".
    std::function<void(std::string reason)> closed;
  };
  virtual ~LobbySocket() = default;
  virtual void Send(std::string text) = 0;
  // Sends a WebSocket ping, whose pong comes as the message "pong". False when
  // the socket can't, and the text "ping" goes instead.
  virtual bool Ping() { return false; }
};
using LobbySocketFactory = std::function<std::unique_ptr<LobbySocket>(
    const std::string& url, LobbySocket::Events events)>;
// How relay transports reach the lobby. Without one they can't. Apple
// platforms start with one over NSURLSession.
void SetLobbySocketFactory(LobbySocketFactory factory);

// Exchanges frames with the other instances in a room the lobby names,
// directly with those whose NATs let a probe through and through a TURN relay
// with the others. A player's local addresses are only tried when the lobby
// saw them behind the same public address, so internet play needs no local
// network access. It joins the lobby once Activate() is called or a frame is
// sent, not before.
std::unique_ptr<PacketTransport> OpenRelayTransport(
    const Mac& mac, const relay::RelayConfig& config, std::string& error);

// Called on the System Link thread when a relay transport's status changes,
// with State::kStopped when it closes.
void SetRelayStatusListener(
    std::function<void(const relay::RelayStatus&)> listener);

}  // namespace xe::net::secure

#endif  // XENIA_NET_PACKET_TRANSPORT_H_
