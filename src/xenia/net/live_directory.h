/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_LIVE_DIRECTORY_H_
#define XENIA_NET_LIVE_DIRECTORY_H_

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "xenia/net/packet_transport.h"

namespace xe::net::live {

// A matchmaking session in the lobby's session directory (lobby protocol
// version 2, "Sessions"). The lobby doesn't look inside |info|.
struct DirectoryAdvert {
  std::string id;  // 16 lower-case hex digits.
  std::vector<uint8_t> info;  // XSESSION_INFO as the title sees it.
  // Room code the session's players meet in; empty for the title's public
  // lobby.
  std::string room;
  uint32_t flags = 0;
  uint32_t public_slots = 0;
  uint32_t private_slots = 0;
  uint32_t used_public = 0;
  uint32_t used_private = 0;
  bool playing = false;
  bool join_in_progress = false;
  uint64_t xuid = 0;
  std::vector<std::pair<uint32_t, uint32_t>> contexts;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> props;
  std::vector<std::pair<uint32_t, std::string>> strings;
};

// The messages, as pure functions for tests.
std::string EncodeAdvertise(const DirectoryAdvert& advert, uint32_t ttl);
std::string EncodeRefresh(uint32_t ttl);
std::string EncodeWithdraw(std::string_view id);
std::string EncodeSearch(
    const std::vector<std::pair<uint32_t, uint32_t>>& contexts, uint32_t max,
    bool in_progress, uint32_t seq);
std::string EncodeLookup(const std::vector<std::string>& ids, uint32_t seq);

struct DirectoryReply {
  enum class Kind { kResults, kAdvertised, kRefreshed, kError, kOther };
  Kind kind = Kind::kOther;
  uint32_t seq = 0;
  std::vector<DirectoryAdvert> sessions;  // kResults.
  std::vector<std::string> ids;  // kAdvertised (one), kRefreshed.
  std::string reason;  // kError.
};
// Nothing for text that isn't a JSON object with a "t". Sessions with a bad
// field are left out of results.
std::optional<DirectoryReply> ParseDirectoryReply(std::string_view text);

std::string HexId(const uint8_t (&id)[8]);

// One title's session directory over a lobby socket. Connects on first use and
// again after the socket closes, advertising again what it advertised. Hosts'
// adverts are refreshed in the background. Thread safe.
class SessionDirectory {
 public:
  // |lobby| is the lobby's ws:// or wss:// address without a path.
  SessionDirectory(std::string lobby, uint32_t title_id);
  ~SessionDirectory();

  void Advertise(const DirectoryAdvert& advert);
  void Withdraw(const std::string& id);
  // Empty when the lobby can't be reached within |timeout_ms|.
  std::vector<DirectoryAdvert> Search(
      const std::vector<std::pair<uint32_t, uint32_t>>& contexts,
      uint32_t max, bool in_progress, uint32_t timeout_ms);
  std::vector<DirectoryAdvert> Lookup(const std::vector<std::string>& ids,
                                      uint32_t timeout_ms);

 private:
  static constexpr uint32_t kTtl = 300;

  // Sends what |encode| makes for a new sequence number and waits for the
  // results that carry it.
  std::vector<DirectoryAdvert> Ask(
      const std::function<std::string(uint32_t seq)>& encode,
      uint32_t timeout_ms);
  void EnsureOpenLocked();
  void OnOpened();
  void OnMessage(std::string text);
  void OnClosed(std::string reason);
  void Run();

  const std::string url_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::unique_ptr<secure::LobbySocket> socket_;
  uint64_t generation_ = 0;  // Bumped when a socket opens or closes.
  bool open_ = false;
  bool closed_ = false;  // The socket closed; Run replaces it.
  bool stopping_ = false;
  uint32_t next_seq_ = 1;
  std::map<uint32_t, std::vector<DirectoryAdvert>> answers_;
  std::map<std::string, DirectoryAdvert> adverts_;
  std::thread thread_;
};

}  // namespace xe::net::live

#endif  // XENIA_NET_LIVE_DIRECTORY_H_
