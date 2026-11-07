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

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
  // Optional text for the list's mode and map columns (up to 64 characters).
  std::string mode;
  std::string map;
  std::vector<std::pair<uint32_t, uint32_t>> contexts;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> props;
  std::vector<std::pair<uint32_t, std::string>> strings;
};

// The lobby's limits on an advert: slots up to 1024, at most 32 contexts and
// 32 properties with distinct ids, 4 strings. The lobby refuses an advert
// past them and closes the socket.
constexpr uint32_t kDirectoryMaxSlots = 1024;
constexpr size_t kDirectoryMaxPairs = 32;
constexpr size_t kDirectoryMaxStrings = 4;
// |advert| cut to the lobby's limits.
DirectoryAdvert FitDirectoryLimits(DirectoryAdvert advert);

// The messages, as pure functions for tests. EncodeAdvertise fits the advert
// to the lobby's limits.
std::string EncodeAdvertise(const DirectoryAdvert& advert, uint32_t ttl);
std::string EncodeRefresh(uint32_t ttl);
// Changes the slots a player joined or left in an advert, and moves its expiry.
std::string EncodeUpdate(std::string_view id, uint32_t used_public,
                         uint32_t used_private, uint32_t ttl);
// The first message on the socket: the account's XUID and a session token.
std::string EncodeDirectoryAuth(uint64_t xuid, std::string_view token);
std::string EncodeWithdraw(std::string_view id);
std::string EncodeSearch(
    const std::vector<std::pair<uint32_t, uint32_t>>& contexts, uint32_t max,
    bool in_progress, uint32_t seq);
std::string EncodeLookup(const std::vector<std::string>& ids, uint32_t seq);

struct DirectoryReply {
  enum class Kind {
    kAuthed,
    kResults,
    kAdvertised,
    kRefreshed,
    kUpdated,
    kError,
    kOther
  };
  Kind kind = Kind::kOther;
  uint32_t seq = 0;
  std::vector<DirectoryAdvert> sessions;  // kResults.
  // kAdvertised (one), kUpdated (one), kRefreshed; kError with an |id|.
  std::vector<std::string> ids;
  std::string reason;  // kError.
  std::string for_request;  // kError: the request it answers, if it says.
  // kError: the lobby refused the socket ("reject": "denied", "banned")
  // rather than a request ("error": "auth_required", ...).
  bool rejected = false;
  std::string appeal;  // kError, "banned": where to appeal.
  uint64_t xuid = 0;  // kAuthed.
  std::string gamertag;  // kAuthed.
};
// Nothing for text that isn't a JSON object with a "t". Sessions with a bad
// field are left out of results. "reject" (a message the lobby refused) is a
// kError.
std::optional<DirectoryReply> ParseDirectoryReply(std::string_view text);

std::string HexId(const uint8_t (&id)[8]);

// The account the directory signs in as. The lobby takes no advert or search
// from a socket that hasn't signed in with a session token.
struct DirectoryAuth {
  // The signed-in account's XUID; 0 when there is none yet.
  std::function<uint64_t()> xuid;
  // A token with time left, if the account has one.
  std::function<std::optional<std::string>()> token;
  // Asks the account for a token; TokenReady() follows when it arrives.
  std::function<void()> request_token;
};

// One title's session directory over a lobby socket. Connects on first use and
// again after the socket closes (waiting longer after each socket that didn't
// last, up to 5 minutes), advertising again what it advertised. Hosts' adverts
// are refreshed in the background, and advertised again when the lobby
// dropped them. Each socket signs in first (|auth|); adverts and searches wait
// for that. Thread safe.
class SessionDirectory {
 public:
  // |lobby| is the lobby's ws:// or wss:// address without a path.
  SessionDirectory(std::string lobby, uint32_t title_id, DirectoryAuth auth);
  ~SessionDirectory();

  void Advertise(const DirectoryAdvert& advert);
  void Withdraw(const std::string& id);
  // A player joined or left: these slots are taken. False when |id| isn't
  // advertised here.
  bool Update(const std::string& id, uint32_t used_public,
              uint32_t used_private);
  // The account has a new token, or signed in: signs in if the socket waits.
  void TokenReady();
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
  // Signs in if the socket is open and waits for that, asking the account for
  // a token when it has none.
  void TryAuthLocked();
  void OnMessage(std::string text);
  void OnClosed(std::string reason);
  void Run();
  // How long Run waits before connecting again.
  std::chrono::milliseconds ReconnectDelayLocked() const;

  const std::string url_;
  const DirectoryAuth auth_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::unique_ptr<secure::LobbySocket> socket_;
  uint64_t generation_ = 0;  // Bumped when a socket opens or closes.
  bool open_ = false;
  bool closed_ = false;  // The socket closed; Run replaces it.
  bool stopping_ = false;
  // The socket signed in (authed): adverts and searches go only then.
  bool authed_ = false;
  bool auth_sent_ = false;
  // The token the socket signed in with, and the one the lobby denied.
  std::string sent_token_;
  std::string denied_token_;
  uint32_t denials_ = 0;
  // The lobby banned the account, or denied a new token too: no more sockets.
  bool halted_ = false;
  std::optional<std::chrono::steady_clock::time_point> token_requested_;
  // Sockets in a row that closed before lasting a minute.
  uint32_t failures_ = 0;
  std::chrono::steady_clock::time_point opened_at_;
  uint32_t next_seq_ = 1;
  // The sequence numbers Ask waits for, and the results that came for them.
  std::set<uint32_t> waiting_;
  std::map<uint32_t, std::vector<DirectoryAdvert>> answers_;
  std::map<std::string, DirectoryAdvert> adverts_;
  std::thread thread_;
};

}  // namespace xe::net::live

#endif  // XENIA_NET_LIVE_DIRECTORY_H_
