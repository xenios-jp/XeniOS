/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/nat_check.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/net/host_interfaces.h"

namespace xe::net::relay {

using stun::Class;
using stun::Method;

std::string_view ToString(NatType type) {
  switch (type) {
    case NatType::kOpen:
      return "open";
    case NatType::kModerate:
      return "moderate";
    case NatType::kStrict:
      return "strict";
    case NatType::kBlocked:
      return "blocked";
    case NatType::kUnknown:
      break;
  }
  return "unknown";
}

std::optional<NatType> ParseNatType(std::string_view text) {
  for (NatType type : {NatType::kUnknown, NatType::kOpen, NatType::kModerate,
                       NatType::kStrict, NatType::kBlocked}) {
    if (text == ToString(type)) {
      return type;
    }
  }
  return std::nullopt;
}

NatType ClassifyNat(const std::optional<Endpoint>& first,
                    const std::optional<Endpoint>& second,
                    std::span<const Endpoint> local) {
  const auto is_local = [&](const Endpoint& mapped) {
    return std::find(local.begin(), local.end(), mapped) != local.end();
  };
  if (!first && !second) {
    return NatType::kBlocked;
  }
  if (!first || !second) {
    // One server can't tell a NAT that keeps its mapping from one that
    // doesn't, only that there's none.
    const Endpoint& mapped = first ? *first : *second;
    return is_local(mapped) ? NatType::kOpen : NatType::kUnknown;
  }
  if (*first != *second) {
    return NatType::kStrict;
  }
  return is_local(*first) ? NatType::kOpen : NatType::kModerate;
}

std::vector<Endpoint> PickNatServers(std::span<const Endpoint> resolved) {
  std::vector<Endpoint> picked;
  for (const Endpoint& endpoint : resolved) {
    if (picked.empty()) {
      picked.push_back(endpoint);
    } else if (endpoint.address != picked[0].address ||
               endpoint.v6 != picked[0].v6) {
      picked.push_back(endpoint);
      return picked;
    }
  }
  // One host: another of its ports, which shows the same thing as long as
  // the NAT maps by port as well as by address.
  for (const Endpoint& endpoint : resolved) {
    if (!picked.empty() && endpoint != picked[0]) {
      picked.push_back(endpoint);
      return picked;
    }
  }
  if (picked.size() == 1) {
    Endpoint other = picked[0];
    other.port = other.port == 3478 ? 19302 : 3478;
    picked.push_back(other);
  }
  return picked;
}

std::string NetworkKey() {
  std::vector<std::string> parts;
  for (const HostInterface& host : HostInterfaces()) {
    parts.push_back(
        fmt::format("{}/{:08x}/{:08x}", host.name, host.address, host.mask));
  }
  std::sort(parts.begin(), parts.end());
  // FNV-1a, so the key doesn't spell out the addresses.
  uint64_t hash = 0xCBF29CE484222325ull;
  for (const std::string& part : parts) {
    for (char c : part) {
      hash = (hash ^ uint8_t(c)) * 0x100000001B3ull;
    }
    hash = (hash ^ uint8_t(';')) * 0x100000001B3ull;
  }
  return fmt::format("{:016x}", hash);
}

namespace {
std::mutex remembered_mutex;
std::map<std::string, NatType> remembered;
}  // namespace

void RememberNat(const std::string& network, NatType type) {
  std::lock_guard lock(remembered_mutex);
  if (type == NatType::kUnknown) {
    remembered.erase(network);
  } else {
    remembered[network] = type;
  }
}

NatType RememberedNat(const std::string& network) {
  std::lock_guard lock(remembered_mutex);
  auto found = remembered.find(network);
  return found == remembered.end() ? NatType::kUnknown : found->second;
}

NatCheck::NatCheck(std::vector<Endpoint> servers, std::vector<Endpoint> local)
    : servers_(std::move(servers)), local_(std::move(local)) {
  servers_.resize(std::min<size_t>(servers_.size(), 2));
}

void NatCheck::Tick(uint64_t now_ms, std::vector<Datagram>& out) {
  if (done()) {
    return;
  }
  if (!started_ms_) {
    started_ms_ = now_ms;
    for (const Endpoint& server : servers_) {
      Ask ask;
      ask.server = server;
      ask.id = stun::NewTransactionId();
      ask.request =
          stun::Writer(Method::kBinding, Class::kRequest, ask.id).Finish();
      ask.next_ms = now_ms;
      asks_.push_back(std::move(ask));
    }
  }
  if (now_ms - *started_ms_ >= kGiveUpMs) {
    timed_out_ = true;
    return;
  }
  for (Ask& ask : asks_) {
    if (ask.mapped || ask.sends >= kSends || now_ms < ask.next_ms) {
      continue;
    }
    out.push_back({ask.server, ask.request});
    ask.next_ms = now_ms + (kFirstRetryMs << ask.sends);
    ++ask.sends;
  }
}

bool NatCheck::Receive(std::span<const uint8_t> datagram,
                       const Endpoint& from) {
  if (asks_.empty() || datagram.size() < stun::kHeaderSize) {
    return false;
  }
  auto message = stun::Read(datagram);
  if (!message || message->method != Method::kBinding) {
    return false;
  }
  for (Ask& ask : asks_) {
    if (ask.id != message->transaction || ask.server != from) {
      continue;
    }
    if (message->kind == Class::kSuccess && message->mapped) {
      ask.mapped = message->mapped;
    }
    return true;
  }
  return false;
}

bool NatCheck::done() const {
  if (servers_.empty() || timed_out_) {
    return true;
  }
  return !asks_.empty() &&
         std::all_of(asks_.begin(), asks_.end(),
                     [](const Ask& ask) { return ask.mapped.has_value(); });
}

NatType NatCheck::result() const {
  if (!done() || asks_.size() < 2) {
    return NatType::kUnknown;
  }
  return ClassifyNat(asks_[0].mapped, asks_[1].mapped, local_);
}

}  // namespace xe::net::relay
