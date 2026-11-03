/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_LOBBY_SOCKET_APPLE_H_
#define XENIA_NET_LOBBY_SOCKET_APPLE_H_

#include <memory>
#include <string>

#include "xenia/net/packet_transport.h"

namespace xe::net::secure {

// A lobby WebSocket over NSURLSession, the default on Apple platforms. None for
// a URL that isn't ws:// or wss://.
std::unique_ptr<LobbySocket> OpenAppleLobbySocket(const std::string& url,
                                                  LobbySocket::Events events);

}  // namespace xe::net::secure

#endif  // XENIA_NET_LOBBY_SOCKET_APPLE_H_
