/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_title_server.h"

#include <charconv>
#include <string_view>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"

DEFINE_string(
    live_title_servers, "",
    "Servers that stand in for titles' own Live servers (LSP), as "
    "TITLEID=IPv4:PORT entries separated by ';', each optionally followed by "
    "|INFO (the server's description). Example: "
    "4D5307E6=192.0.2.1:8000. Only used by a profile signed in to Live.",
    "Live");

namespace xe {
namespace kernel {
namespace xam {

namespace {

// Titles whose servers are told apart by the description: it lists the
// services the server provides, and the title refuses a server without them.
// TODO(live): 61 titles on the test SSD import the title-server calls
// (results/2026-11/live/lsp-scan). Only Bungie's are listed here; add others
// as servers for them appear, from what each title's enumerator expects.
std::string_view DefaultInfo(uint32_t title_id) {
  switch (title_id) {
    case 0x4D5307E6:  // Halo 3
    case 0x4D53880C:  // Halo 3 public beta
    case 0x4D53883A:  // Halo 3 internal beta
    case 0x4D530877:  // Halo 3: ODST
    case 0x4D53085B:  // Halo: Reach
      return "required,mass_storage,other,ttl,usr,shr,web,dbg,upl,prs,std";
    default:
      return "";
  }
}

std::optional<uint32_t> ParseIpv4(std::string_view text) {
  uint32_t address = 0;
  for (int i = 0; i < 4; ++i) {
    unsigned octet = 0;
    auto [end, ec] =
        std::from_chars(text.data(), text.data() + text.size(), octet);
    if (ec != std::errc() || octet > 255) {
      return std::nullopt;
    }
    address = (address << 8) | octet;
    text.remove_prefix(end - text.data());
    if (i < 3) {
      if (text.empty() || text.front() != '.') {
        return std::nullopt;
      }
      text.remove_prefix(1);
    }
  }
  if (!text.empty()) {
    return std::nullopt;
  }
  return address;
}

std::optional<TitleServer> ParseEntry(std::string_view entry,
                                      uint32_t title_id) {
  const size_t eq = entry.find('=');
  if (eq == std::string_view::npos) {
    return std::nullopt;
  }
  uint32_t entry_title = 0;
  auto [title_end, title_ec] = std::from_chars(
      entry.data(), entry.data() + eq, entry_title, 16);
  if (title_ec != std::errc() || title_end != entry.data() + eq ||
      entry_title != title_id) {
    return std::nullopt;
  }
  std::string_view rest = entry.substr(eq + 1);
  std::string_view info = DefaultInfo(title_id);
  if (const size_t bar = rest.find('|'); bar != std::string_view::npos) {
    info = rest.substr(bar + 1);
    rest = rest.substr(0, bar);
  }
  const size_t colon = rest.rfind(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  const auto address = ParseIpv4(rest.substr(0, colon));
  uint32_t port = 0;
  const std::string_view port_text = rest.substr(colon + 1);
  auto [port_end, port_ec] = std::from_chars(
      port_text.data(), port_text.data() + port_text.size(), port);
  if (!address || port_ec != std::errc() ||
      port_end != port_text.data() + port_text.size() || !port ||
      port > 0xFFFF) {
    return std::nullopt;
  }
  TitleServer server;
  server.address = *address;
  server.port = static_cast<uint16_t>(port);
  server.info = std::string(info.substr(0, kTitleServerInfoTextSize - 1));
  return server;
}

}  // namespace

std::optional<TitleServer> GetTitleServer() {
  const std::string& setting = cvars::live_title_servers;
  if (setting.empty()) {
    return std::nullopt;
  }
  const uint32_t title_id = kernel_state()->title_id();
  std::string_view rest = setting;
  while (!rest.empty()) {
    const size_t end = rest.find(';');
    const std::string_view entry = rest.substr(0, end);
    if (auto server = ParseEntry(entry, title_id)) {
      return server;
    }
    if (end == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(end + 1);
  }
  return std::nullopt;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
