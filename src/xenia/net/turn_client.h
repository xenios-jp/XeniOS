/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_TURN_CLIENT_H_
#define XENIA_NET_TURN_CLIENT_H_

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "xenia/net/relay_protocol.h"
#include "xenia/net/stun.h"

namespace xe::net::relay {

// Learns where a STUN or TURN server sees this socket from and, given TURN
// credentials, holds an allocation there that relays to and from peers.
// Without a socket or a clock, like RelayClient.
class TurnClient {
 public:
  struct Credentials {
    std::string username;
    std::string password;
  };

  // STUN only without |credentials|.
  TurnClient(const Endpoint& server, std::optional<Credentials> credentials);

  const Endpoint& server() const { return server_; }
  // Where the server sees this socket from.
  const std::optional<Endpoint>& mapped() const { return mapped_; }
  // The allocation's address, which peers send to.
  const std::optional<Endpoint>& relayed() const { return relayed_; }
  enum class Failure {
    kNone,
    // Requests went unanswered.
    kSilent,
    // The allocation went away, or this socket's address at the server
    // changed, which ends it.
    kLost,
    // The server holds an allocation for this socket that the credentials
    // can't free, until it expires.
    kInUse,
    // The server turned the credentials or the allocation down.
    kRefused,
    kReleased,
  };
  // Why the client is no use any more, if it isn't.
  Failure failure() const { return failure_; }
  bool failed() const { return failure_ != Failure::kNone; }

  // Sends the requests that are due. Call every few tens of ms.
  void Tick(uint64_t now_ms, std::vector<Datagram>& out);

  struct Relayed {
    Endpoint peer;
    std::vector<uint8_t> data;
  };
  // Takes a datagram from the server. What a peer sent to the allocation
  // comes back out.
  std::optional<Relayed> Receive(std::span<const uint8_t> datagram,
                                 uint64_t now_ms, std::vector<Datagram>& out);

  // Lets the addresses of |peers| (not their ports) send to the allocation,
  // and the allocation send to them.
  void Permit(const std::vector<Endpoint>& peers, uint64_t now_ms,
              std::vector<Datagram>& out);
  // False without an allocation or a permission for |peer| yet.
  bool Send(const Endpoint& peer, std::span<const uint8_t> data,
            std::vector<Datagram>& out);

  // Frees the allocation, without waiting for an answer.
  void Release(std::vector<Datagram>& out);

 private:
  // RFC 8489's 500 ms, doubling, without the last waits: a server that
  // hasn't answered in 15 s is given up.
  static constexpr uint64_t kFirstRetryMs = 500;
  static constexpr uint32_t kSends = 5;
  static constexpr uint64_t kGiveUpMs = 15500;
  // Keeps the NAT mapping to the server open and notices when it changes.
  static constexpr uint64_t kKeepaliveMs = 15000;
  // Permissions last 300 s.
  static constexpr uint64_t kPermissionRefreshMs = 240000;
  static constexpr uint32_t kMaxPermissions = 32;

  enum Kind { kBind, kAllocate, kRefresh, kPermission, kKinds };
  struct Pending {
    stun::TransactionId id{};
    std::vector<uint8_t> request;
    uint64_t sent_ms = 0;
    uint64_t next_ms = 0;
    uint64_t first_ms = 0;
    uint32_t sends = 0;
    // The addresses a permission request asks for.
    std::set<std::array<uint8_t, 17>> addresses;
  };

  static std::array<uint8_t, 17> AddressKey(const Endpoint& endpoint);
  void Request(Kind kind, std::vector<uint8_t> request, uint64_t now_ms,
               std::vector<Datagram>& out,
               std::set<std::array<uint8_t, 17>> addresses = {});
  // Adds the credentials and MESSAGE-INTEGRITY.
  std::vector<uint8_t> Sign(stun::Writer&& writer) const;
  void SendAllocate(uint64_t now_ms, std::vector<Datagram>& out);
  // Frees an allocation this socket made before, so another can be made.
  void SendClear(uint64_t now_ms, std::vector<Datagram>& out);
  void SendPermissions(uint64_t now_ms, std::vector<Datagram>& out);
  void OnResponse(Kind kind, const Pending& answered,
                  const stun::Message& message, uint64_t now_ms,
                  std::vector<Datagram>& out);
  void Fail(const char* why, Failure failure);

  Endpoint server_;
  std::optional<Credentials> credentials_;
  std::string realm_;
  std::string nonce_;
  std::array<uint8_t, 16> key_{};
  bool keyed_ = false;
  uint32_t nonce_retries_ = 0;
  bool cleared_ = false;

  std::optional<Endpoint> mapped_;
  std::optional<Endpoint> relayed_;
  Failure failure_ = Failure::kNone;
  bool started_ = false;
  uint64_t next_keepalive_ms_ = 0;
  uint64_t refresh_ms_ = 0;

  std::set<std::array<uint8_t, 17>> wanted_;
  std::set<std::array<uint8_t, 17>> permitted_;
  std::vector<Endpoint> wanted_endpoints_;
  uint64_t permitted_ms_ = 0;
  uint64_t permission_retry_ms_ = 0;

  std::array<std::optional<Pending>, kKinds> pending_;
};

}  // namespace xe::net::relay

#endif  // XENIA_NET_TURN_CLIENT_H_
