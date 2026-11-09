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

// Stats: what a flush changes in the player's own
// rows, as column updates the title's SPA describes, and what reads return.
enum class StatsOp : uint8_t { kSum, kMax, kMin, kLast };

// One XUSER_DATA value of a stats column. |type| is the X_USER_DATA_TYPE: 0
// context, 1 int32, 2 int64, 3 double, 5 float, 7 datetime. Integers are in
// |i|, floating point in |d|.
struct StatsValue {
  uint8_t type = 0;
  int64_t i = 0;
  double d = 0;
  bool operator==(const StatsValue&) const = default;
};

constexpr uint16_t kStatsAttrRank = 0xFFFF;
constexpr uint16_t kStatsAttrRating = 0xFFFE;
constexpr uint16_t kStatsAttrGamertag = 0xFFFD;
constexpr size_t kStatsMaxWriteViews = 16;
constexpr size_t kStatsMaxColumns = 64;
constexpr size_t kStatsMaxReadViews = 8;
constexpr size_t kStatsMaxReadXuids = 101;
constexpr uint32_t kStatsMaxEnumRows = 100;

struct StatsColumnUpdate {
  uint16_t attr = 0;
  StatsOp op = StatsOp::kLast;
  StatsValue value;
};

// A view a flush changes. A TrueSkill view (0xFFFE.... or 0xFFFF....) has no
// columns: it counts a game.
struct StatsViewWrite {
  uint32_t view_id = 0;
  std::vector<StatsColumnUpdate> columns;
};

struct StatsSpec {
  uint32_t view_id = 0;
  std::vector<uint16_t> columns;
};

struct StatsCell {
  uint16_t attr = 0;
  StatsValue value;
};

struct StatsRowData {
  uint64_t xuid = 0;
  std::string gamertag;
  uint32_t rank = 0;
  int64_t rating = 0;
  std::vector<StatsCell> cells;
};

struct StatsViewData {
  uint32_t view_id = 0;
  uint32_t total = 0;
  std::vector<StatsRowData> rows;
};

enum class StatsEnumBy { kRank, kXuid, kRating };

std::string EncodeStatsWrite(const std::vector<StatsViewWrite>& views,
                             uint32_t seq);
std::string EncodeStatsRead(const std::vector<uint64_t>& xuids,
                            const std::vector<StatsSpec>& specs, uint32_t seq);
// |pivot| is the first rank, the player's XUID, or the rating (as int64).
std::string EncodeStatsEnum(StatsEnumBy by, uint64_t pivot, uint32_t rows,
                            const std::vector<StatsSpec>& specs, uint32_t seq);

struct DirectoryReply {
  enum class Kind {
    kAuthed,
    kResults,
    kAdvertised,
    kRefreshed,
    kUpdated,
    kStats,
    kStatsWritten,
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
  uint32_t stats = 0;  // kAuthed: the lobby's stats version, 0 for none.
  std::vector<StatsViewData> stats_views;  // kStats.
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

  // Queues the signed-in account's stats from a flush; they go once the
  // socket is signed in to a lobby that keeps stats. Returns at once.
  void WriteStats(std::vector<StatsViewWrite> views);
  // XUserReadStats and the stats enumerators. Nothing when the lobby has no
  // stats, the account isn't signed in, or no answer came in time. The same
  // read within 30 s is answered from memory, until this app writes stats.
  std::optional<std::vector<StatsViewData>> ReadStats(
      const std::vector<uint64_t>& xuids, const std::vector<StatsSpec>& specs,
      uint32_t timeout_ms);
  std::optional<std::vector<StatsViewData>> EnumerateStats(
      StatsEnumBy by, uint64_t pivot, uint32_t rows,
      const std::vector<StatsSpec>& specs, uint32_t timeout_ms);
  // Writes queued or sent and not yet answered (for tests).
  size_t PendingStatsWrites();

 private:
  static constexpr uint32_t kTtl = 300;

  // Sends what |encode| makes for a new sequence number and waits for the
  // results that carry it.
  std::vector<DirectoryAdvert> Ask(
      const std::function<std::string(uint32_t seq)>& encode,
      uint32_t timeout_ms);
  std::optional<std::vector<StatsViewData>> AskStats(
      const std::function<std::string(uint32_t seq)>& encode,
      uint32_t timeout_ms);
  void SendStatsWritesLocked();
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
  // The device's network changed (Run acts on it).
  bool network_changed_ = false;
  uint64_t network_listener_ = 0;
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
  // The lobby's stats version from its last authed, 0 for none.
  uint32_t stats_version_ = 0;
  // Answers to stats reads by seq; nothing for one the lobby refused.
  std::map<uint32_t, std::optional<std::vector<StatsViewData>>> stats_answers_;
  // Flushes not yet answered, oldest first, with the seq they went with (0:
  // not sent on this socket).
  struct PendingWrite {
    std::vector<StatsViewWrite> views;
    uint32_t seq = 0;
  };
  std::vector<PendingWrite> stats_writes_;
  struct CachedRead {
    std::chrono::steady_clock::time_point at;
    std::vector<StatsViewData> views;
  };
  std::map<std::string, CachedRead> stats_cache_;
  std::thread thread_;
};

}  // namespace xe::net::live

#endif  // XENIA_NET_LIVE_DIRECTORY_H_
