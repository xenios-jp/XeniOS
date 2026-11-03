/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/turn_client.h"

#include <algorithm>

#include "xenia/base/logging.h"

namespace xe::net::relay {
namespace {

using stun::Attribute;
using stun::Class;
using stun::Method;

constexpr uint32_t kLifetimeS = 600;
constexpr uint32_t kNonceRetries = 3;
// A refused permission request is asked again after this.
constexpr uint64_t kPermissionRetryMs = 5000;

uint64_t RefreshDelayMs(uint32_t lifetime_s) {
  // A minute early, or halfway through a short allocation.
  return lifetime_s > 120
             ? uint64_t(lifetime_s - 60) * 1000
             : std::max<uint64_t>(uint64_t(lifetime_s) * 500, 1000);
}

}  // namespace

TurnClient::TurnClient(const Endpoint& server,
                       std::optional<Credentials> credentials)
    : server_(server), credentials_(std::move(credentials)) {}

std::array<uint8_t, 17> TurnClient::AddressKey(const Endpoint& endpoint) {
  std::array<uint8_t, 17> key{};
  key[0] = endpoint.v6;
  std::copy(endpoint.address.begin(), endpoint.address.end(), key.begin() + 1);
  return key;
}

void TurnClient::Request(Kind kind, std::vector<uint8_t> request,
                         uint64_t now_ms, std::vector<Datagram>& out,
                         std::set<std::array<uint8_t, 17>> addresses) {
  Pending pending;
  std::copy_n(request.begin() + 8, pending.id.size(), pending.id.begin());
  pending.request = std::move(request);
  pending.first_ms = now_ms;
  pending.next_ms = now_ms + kFirstRetryMs;
  pending.sends = 1;
  pending.addresses = std::move(addresses);
  out.push_back({server_, pending.request});
  pending_[kind] = std::move(pending);
}

std::vector<uint8_t> TurnClient::Sign(stun::Writer&& writer) const {
  writer.Add(Attribute::kUsername, credentials_->username)
      .Add(Attribute::kRealm, realm_)
      .Add(Attribute::kNonce, nonce_);
  return writer.Finish(key_);
}

void TurnClient::SendAllocate(uint64_t now_ms, std::vector<Datagram>& out) {
  stun::Writer writer(Method::kAllocate, Class::kRequest,
                      stun::NewTransactionId());
  const uint8_t transport[] = {stun::kTransportUdp, 0, 0, 0};
  writer.Add(Attribute::kRequestedTransport, transport);
  // The first one goes without credentials, to learn the realm and nonce.
  Request(kAllocate, keyed_ ? Sign(std::move(writer)) : writer.Finish(), now_ms,
          out);
}

void TurnClient::SendClear(uint64_t now_ms, std::vector<Datagram>& out) {
  stun::Writer writer(Method::kRefresh, Class::kRequest,
                      stun::NewTransactionId());
  writer.AddUint32(Attribute::kLifetime, 0);
  Request(kRefresh, Sign(std::move(writer)), now_ms, out);
}

void TurnClient::SendPermissions(uint64_t now_ms, std::vector<Datagram>& out) {
  if (wanted_endpoints_.empty()) {
    return;
  }
  stun::Writer writer(Method::kCreatePermission, Class::kRequest,
                      stun::NewTransactionId());
  for (const Endpoint& peer : wanted_endpoints_) {
    writer.AddAddress(Attribute::kXorPeerAddress, peer);
  }
  Request(kPermission, Sign(std::move(writer)), now_ms, out, wanted_);
}

void TurnClient::Tick(uint64_t now_ms, std::vector<Datagram>& out) {
  if (failed()) {
    return;
  }
  if (!started_) {
    started_ = true;
    next_keepalive_ms_ = now_ms + kKeepaliveMs;
    if (credentials_) {
      SendAllocate(now_ms, out);
    } else {
      Request(kBind,
              stun::Writer(Method::kBinding, Class::kRequest,
                           stun::NewTransactionId())
                  .Finish(),
              now_ms, out);
    }
    return;
  }
  for (size_t kind = 0; kind < kKinds; ++kind) {
    auto& pending = pending_[kind];
    if (!pending || now_ms < pending->next_ms) {
      continue;
    }
    if (pending->sends < kSends) {
      out.push_back({server_, pending->request});
      pending->next_ms = now_ms + (kFirstRetryMs << pending->sends);
      ++pending->sends;
      continue;
    }
    pending.reset();
    if (kind == kPermission) {
      permission_retry_ms_ = now_ms + kPermissionRetryMs;
    } else {
      Fail("the server stopped answering", Failure::kSilent);
      return;
    }
  }
  const bool ready = !credentials_ || relayed_;
  if (ready && !pending_[kBind] && now_ms >= next_keepalive_ms_) {
    next_keepalive_ms_ = now_ms + kKeepaliveMs;
    Request(kBind,
            stun::Writer(Method::kBinding, Class::kRequest,
                         stun::NewTransactionId())
                .Finish(),
            now_ms, out);
  }
  if (!relayed_) {
    return;
  }
  if (!pending_[kRefresh] && now_ms >= refresh_ms_) {
    stun::Writer writer(Method::kRefresh, Class::kRequest,
                        stun::NewTransactionId());
    writer.AddUint32(Attribute::kLifetime, kLifetimeS);
    Request(kRefresh, Sign(std::move(writer)), now_ms, out);
  }
  const bool stale =
      !permitted_.empty() && now_ms - permitted_ms_ >= kPermissionRefreshMs;
  if (!pending_[kPermission] && now_ms >= permission_retry_ms_ &&
      (stale || !std::includes(permitted_.begin(), permitted_.end(),
                               wanted_.begin(), wanted_.end()))) {
    SendPermissions(now_ms, out);
  }
}

std::optional<TurnClient::Relayed> TurnClient::Receive(
    std::span<const uint8_t> datagram, uint64_t now_ms,
    std::vector<Datagram>& out) {
  auto message = stun::Read(datagram);
  if (!message || failed()) {
    return std::nullopt;
  }
  if (message->kind == Class::kIndication) {
    if (message->method == Method::kData && message->peer && relayed_) {
      return Relayed{*message->peer, std::move(message->data)};
    }
    return std::nullopt;
  }
  if (message->kind != Class::kSuccess && message->kind != Class::kError) {
    return std::nullopt;
  }
  for (size_t kind = 0; kind < kKinds; ++kind) {
    auto& pending = pending_[kind];
    if (!pending || pending->id != message->transaction) {
      continue;
    }
    // Only the server can sign with the key. Once there is one, only the
    // errors that hand out a realm and nonce may come unsigned.
    if (keyed_ && kind != kBind &&
        (message->integrity
             ? !stun::CheckIntegrity(datagram, *message, key_)
             : message->kind == Class::kSuccess ||
                   (message->error != 401 && message->error != 438))) {
      return std::nullopt;
    }
    Pending answered = std::move(*pending);
    pending.reset();
    OnResponse(Kind(kind), answered, *message, now_ms, out);
    break;
  }
  return std::nullopt;
}

void TurnClient::OnResponse(Kind kind, const Pending& answered,
                            const stun::Message& message, uint64_t now_ms,
                            std::vector<Datagram>& out) {
  // A stale nonce is replaced and the request asked again.
  if (message.kind == Class::kError && message.error == 438 &&
      !message.nonce.empty() && keyed_ && kind != kBind) {
    if (++nonce_retries_ > kNonceRetries) {
      Fail("the server keeps refusing its nonces", Failure::kRefused);
      return;
    }
    nonce_ = message.nonce;
    if (kind == kAllocate) {
      SendAllocate(now_ms, out);
    } else if (kind == kRefresh && !relayed_) {
      SendClear(now_ms, out);
    } else if (kind == kRefresh) {
      refresh_ms_ = now_ms;
    } else {
      permission_retry_ms_ = now_ms;
    }
    return;
  }
  if (message.kind == Class::kSuccess && kind != kBind) {
    nonce_retries_ = 0;
  }
  switch (kind) {
    case kBind:
      if (message.kind == Class::kSuccess && message.mapped) {
        // An allocation belongs to the address it was made from.
        if (relayed_ && mapped_ && *message.mapped != *mapped_) {
          Fail("this device's address changed", Failure::kLost);
          return;
        }
        mapped_ = message.mapped;
      }
      break;
    case kAllocate:
      if (message.kind == Class::kSuccess) {
        if (!message.relayed || message.lifetime == 0u) {
          Fail("the allocation came back empty", Failure::kLost);
          return;
        }
        relayed_ = message.relayed;
        if (message.mapped) {
          mapped_ = message.mapped;
        }
        refresh_ms_ =
            now_ms + RefreshDelayMs(message.lifetime.value_or(kLifetimeS));
        next_keepalive_ms_ = now_ms + kKeepaliveMs;
        XELOGI("System Link: relaying through {}", ToString(*relayed_));
        SendPermissions(now_ms, out);
      } else if (message.error == 401 && !keyed_ && credentials_ &&
                 !message.realm.empty() && !message.nonce.empty()) {
        realm_ = message.realm;
        nonce_ = message.nonce;
        key_ = stun::LongTermKey(credentials_->username, realm_,
                                 credentials_->password);
        keyed_ = true;
        SendAllocate(now_ms, out);
      } else if (message.error == 437 && keyed_ && !cleared_) {
        // Most likely this socket's own, from before the server went quiet.
        cleared_ = true;
        SendClear(now_ms, out);
      } else if (message.error == 437) {
        Fail("the server still has an allocation for this device",
             Failure::kInUse);
      } else {
        Fail(message.error == 401 ? "the credentials were refused"
                                  : "the allocation was refused",
             Failure::kRefused);
      }
      break;
    case kRefresh:
      if (!relayed_) {
        // Freeing the old allocation.
        if (message.kind == Class::kSuccess) {
          SendAllocate(now_ms, out);
        } else {
          Fail("the server still has an allocation for this device",
               Failure::kInUse);
        }
      } else if (message.kind == Class::kSuccess && message.lifetime != 0u) {
        refresh_ms_ =
            now_ms + RefreshDelayMs(message.lifetime.value_or(kLifetimeS));
      } else if (message.error == 401) {
        Fail("the credentials expired", Failure::kRefused);
      } else {
        Fail("the allocation was lost", Failure::kLost);
      }
      break;
    case kPermission:
      if (message.kind == Class::kSuccess) {
        permitted_ = answered.addresses;
        permitted_ms_ = now_ms;
      } else {
        permission_retry_ms_ = now_ms + kPermissionRetryMs;
      }
      break;
    case kKinds:
      break;
  }
}

void TurnClient::Permit(const std::vector<Endpoint>& peers, uint64_t now_ms,
                        std::vector<Datagram>& out) {
  std::set<std::array<uint8_t, 17>> wanted;
  std::vector<Endpoint> endpoints;
  for (const Endpoint& peer : peers) {
    if (wanted.size() < kMaxPermissions &&
        wanted.insert(AddressKey(peer)).second) {
      endpoints.push_back(peer);
    }
  }
  if (wanted == wanted_) {
    return;
  }
  wanted_ = std::move(wanted);
  wanted_endpoints_ = std::move(endpoints);
  if (relayed_ && !pending_[kPermission] &&
      !std::includes(permitted_.begin(), permitted_.end(), wanted_.begin(),
                     wanted_.end())) {
    permission_retry_ms_ = 0;
    SendPermissions(now_ms, out);
  }
}

bool TurnClient::Send(const Endpoint& peer, std::span<const uint8_t> data,
                      std::vector<Datagram>& out) {
  if (!relayed_ || !permitted_.count(AddressKey(peer))) {
    return false;
  }
  out.push_back({server_, stun::Writer(Method::kSend, Class::kIndication,
                                       stun::NewTransactionId())
                              .AddAddress(Attribute::kXorPeerAddress, peer)
                              .Add(Attribute::kData, data)
                              .Finish()});
  return true;
}

void TurnClient::Release(std::vector<Datagram>& out) {
  if (relayed_ && keyed_ && !failed()) {
    stun::Writer writer(Method::kRefresh, Class::kRequest,
                        stun::NewTransactionId());
    writer.AddUint32(Attribute::kLifetime, 0);
    // Twice, as nothing waits for the answer, and an allocation that outlives
    // it keeps this socket from making another until it expires.
    out.push_back({server_, Sign(std::move(writer))});
    out.push_back(out.back());
  }
  relayed_.reset();
  pending_ = {};
  failure_ = Failure::kReleased;
}

void TurnClient::Fail(const char* why, Failure failure) {
  XELOGW("System Link: the TURN server at {}: {}", ToString(server_), why);
  failure_ = failure;
  relayed_.reset();
  pending_ = {};
}

}  // namespace xe::net::relay
