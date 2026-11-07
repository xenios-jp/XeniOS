/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_TITLE_SERVER_H_
#define XENIA_KERNEL_XAM_XAM_TITLE_SERVER_H_

#include <cstdint>
#include <optional>
#include <string>

namespace xe {
namespace kernel {
namespace xam {

// A title's own Live servers (its LSP), which Live used to hand out through
// XTitleServerCreateEnumerator. Xenia's Live points the title at a server set
// in the live_title_servers cvar instead, such as a community revival.
struct TitleServer {
  uint32_t address;  // IPv4, host order.
  uint16_t port;     // Every TCP connection to the address goes to this port.
  std::string info;  // XTITLE_SERVER_INFO szServerInfo.
};

// The server set for the running title, if any.
// TODO(xenios-jp): community revival servers by default (Sunrise for Halo 3 and
// Halo: Reach).
std::optional<TitleServer> GetTitleServer();

// XamCreateEnumeratorHandle open message of XTitleServerCreateEnumerator.
constexpr uint32_t kTitleServerEnumerateMessage = 0x58039;

// XTITLE_SERVER_INFO.
constexpr uint32_t kTitleServerInfoSize = 0xD0;
constexpr uint32_t kTitleServerInfoTextSize = 200;

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_TITLE_SERVER_H_
