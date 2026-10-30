/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/host_interfaces.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

#include "xenia/base/platform.h"

#if XE_PLATFORM_APPLE
#include <net/if_dl.h>
#else
#include <netpacket/packet.h>
#endif

namespace xe::net {
namespace {

uint32_t Ipv4(const sockaddr* address) {
  sockaddr_in full{};
#if XE_PLATFORM_APPLE
  // BSD trims trailing zero bytes off a netmask so it can end early.
  std::memcpy(&full, address, std::min<size_t>(address->sa_len, sizeof(full)));
#else
  std::memcpy(&full, address, sizeof(full));
#endif
  return ntohl(full.sin_addr.s_addr);
}

// The link layer address of an interface comes as an entry of its own.
bool LinkAddress(const sockaddr* address, std::array<uint8_t, 6>& mac) {
#if XE_PLATFORM_APPLE
  if (address->sa_family != AF_LINK) {
    return false;
  }
  const auto* link = reinterpret_cast<const sockaddr_dl*>(address);
  if (link->sdl_alen != mac.size()) {
    return false;
  }
  std::memcpy(mac.data(), LLADDR(link), mac.size());
#else
  if (address->sa_family != AF_PACKET) {
    return false;
  }
  const auto* link = reinterpret_cast<const sockaddr_ll*>(address);
  if (link->sll_halen != mac.size()) {
    return false;
  }
  std::memcpy(mac.data(), link->sll_addr, mac.size());
#endif
  return true;
}

}  // namespace

std::vector<HostInterface> HostInterfaces() {
  std::vector<HostInterface> result;
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces)) {
    return result;
  }
  std::map<std::string, std::array<uint8_t, 6>> macs;
  for (auto* entry = interfaces; entry; entry = entry->ifa_next) {
    std::array<uint8_t, 6> mac;
    if (entry->ifa_addr && LinkAddress(entry->ifa_addr, mac)) {
      macs[entry->ifa_name] = mac;
    }
  }
  for (auto* entry = interfaces; entry; entry = entry->ifa_next) {
    if (!entry->ifa_addr || !entry->ifa_netmask ||
        entry->ifa_addr->sa_family != AF_INET || !(entry->ifa_flags & IFF_UP) ||
        !(entry->ifa_flags & IFF_RUNNING) ||
        !(entry->ifa_flags & IFF_BROADCAST) ||
        (entry->ifa_flags & IFF_LOOPBACK)) {
      continue;
    }
    HostInterface host;
    host.name = entry->ifa_name;
    host.address = Ipv4(entry->ifa_addr);
    host.mask = Ipv4(entry->ifa_netmask);
    host.index = if_nametoindex(entry->ifa_name);
    auto mac = macs.find(entry->ifa_name);
    if (mac != macs.end()) {
      host.mac = mac->second;
    }
    result.push_back(host);
  }
  freeifaddrs(interfaces);
  return result;
}

}  // namespace xe::net
