/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_live_invites.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_live_party.h"
#include "xenia/kernel/xam/xam_live_social.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/net/xnet_association.h"

DEFINE_bool(live_auto_accept_invites, false,
            "Accept every game invite from a friend as it comes, as if the "
            "player chose Accept. For tests without the UI.",
            "Live");
DEFINE_string(live_test_invite_xuid, "",
              "Online XUID (hex) the first signed-in profile invites to its "
              "game once it's in a session friends may join and the XUID is "
              "its friend. For tests: some titles only invite from the Guide.",
              "Live");

DEFINE_bool(live_test_accept_again, false,
            "With live_auto_accept_invites, accept each invite a second time "
            "3 s later, as a second tap on Accept would. For tests.",
            "Live");
DEFINE_bool(live_test_invite_system_link, false,
            "With live_test_invite_xuid, send the test invite from a System "
            "Link game (a session without Live presence, so only the room to "
            "join) instead of from a Live session. For tests.",
            "Live");
// Transient: never in the config file, so it starts one launch only.
DEFINE_transient_string(live_accepted_invite, "",
              "An invite the player accepted in another title, which started "
              "this one for it (EncodeAcceptedInvite). Set by the app when it "
              "relaunches; the title reads it with XInviteGetAcceptedInfo.",
              "Live");
DEFINE_int32(live_test_invite_again_s, 0,
             "With live_test_invite_xuid, send the test invite once more this "
             "many seconds after the first (0: never). For tests.",
             "Live");

namespace xe {
namespace kernel {
namespace xam {

std::optional<InviteSendRequest> ParseInviteSendRequest(
    std::span<const uint8_t> bytes) {
  size_t offset = 0;
  auto u32 = [&](uint32_t& value) {
    if (bytes.size() - offset < 4) {
      return false;
    }
    value = xe::load_and_swap<uint32_t>(bytes.data() + offset);
    offset += 4;
    return true;
  };
  InviteSendRequest request;
  uint32_t count = 0;
  if (!u32(request.user_index) || !u32(count) || count > kInviteMaxInvitees ||
      bytes.size() - offset < size_t(count) * 8) {
    return std::nullopt;
  }
  for (uint32_t i = 0; i < count; ++i) {
    request.invitees.push_back(
        xe::load_and_swap<uint64_t>(bytes.data() + offset));
    offset += 8;
  }
  uint32_t length = 0;
  if (!u32(length) || length > kInviteMaxText ||
      bytes.size() - offset < size_t(length) * 2) {
    return std::nullopt;
  }
  for (uint32_t i = 0; i < length; ++i) {
    const auto c = static_cast<char16_t>(
        xe::load_and_swap<uint16_t>(bytes.data() + offset));
    offset += 2;
    if (!c) {
      // The length may count the terminator.
      offset += size_t(length - i - 1) * 2;
      break;
    }
    request.text.push_back(c);
  }
  if (!u32(request.message_handle)) {
    return std::nullopt;
  }
  return request;
}

bool FillInviteInfo(const AcceptedInvite& invite, X_INVITE_INFO* out) {
  std::memset(out, 0, sizeof(*out));
  if (invite.info.size() != sizeof(XSESSION_INFO)) {
    return false;
  }
  out->xuid_invitee = invite.invitee;
  out->xuid_inviter = invite.inviter;
  out->title_id = invite.title_id;
  std::memcpy(&out->host_info, invite.info.data(), sizeof(XSESSION_INFO));
  out->from_game_invite = invite.from_game_invite ? 1 : 0;
  return true;
}

std::string EncodeAcceptedInvite(uint32_t user_index,
                                const AcceptedInvite& invite,
                                const std::string& room) {
  std::string info;
  for (const uint8_t b : invite.info) {
    info += fmt::format("{:02x}", b);
  }
  return fmt::format("{}:{:016X}:{:016X}:{:08X}:{}:{}:{}", user_index,
                     invite.invitee, invite.inviter, invite.title_id,
                     invite.from_game_invite ? 1 : 0, info, room);
}

std::optional<LaunchInvite> DecodeAcceptedInvite(std::string_view text) {
  std::vector<std::string_view> fields;
  for (size_t at = 0;;) {
    const size_t colon = text.find(':', at);
    fields.push_back(text.substr(at, colon - at));
    if (colon == std::string_view::npos) {
      break;
    }
    at = colon + 1;
  }
  if (fields.size() != 7) {
    return std::nullopt;
  }
  auto hex = [](std::string_view field, size_t max_digits, uint64_t& out) {
    if (field.empty() || field.size() > max_digits) {
      return false;
    }
    out = 0;
    for (const char c : field) {
      const int digit = c >= '0' && c <= '9'   ? c - '0'
                        : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                               : -1;
      if (digit < 0) {
        return false;
      }
      out = out << 4 | uint64_t(digit);
    }
    return true;
  };
  LaunchInvite launch;
  uint64_t user = 0, title = 0, from_game = 0;
  if (!hex(fields[0], 1, user) || user >= XUserMaxUserCount ||
      !hex(fields[1], 16, launch.invite.invitee) ||
      !hex(fields[2], 16, launch.invite.inviter) || !hex(fields[3], 8, title) ||
      !title || !hex(fields[4], 1, from_game) || from_game > 1) {
    return std::nullopt;
  }
  launch.user_index = uint32_t(user);
  launch.invite.title_id = uint32_t(title);
  launch.invite.from_game_invite = from_game != 0;
  // The session, or none (a System Link game: only the room).
  const auto info = fields[5];
  if (!info.empty()) {
    if (info.size() != sizeof(XSESSION_INFO) * 2) {
      return std::nullopt;
    }
    for (size_t i = 0; i < info.size(); i += 2) {
      uint64_t byte;
      if (!hex(info.substr(i, 2), 2, byte)) {
        return std::nullopt;
      }
      launch.invite.info.push_back(uint8_t(byte));
    }
  }
  launch.room = std::string(fields[6]);
  return launch;
}

namespace {

struct Invites {
  std::mutex mutex;
  KernelState* kernel_state = nullptr;
  std::array<std::optional<net::live::Invite>, XUserMaxUserCount> pending;
  std::array<std::optional<AcceptedInvite>, XUserMaxUserCount> accepted;
  // The kernel each accepted invite is for: the running one's, or null for
  // the title that starts next (accepted in the library). A title that ends
  // without reading its invite takes it with it.
  std::array<KernelState*, XUserMaxUserCount> accepted_for{};
  // The invites already accepted (newest last): accepting one again doesn't
  // join again.
  std::vector<std::string> answered;
  // The session the test invite went out for.
  std::array<std::string, XUserMaxUserCount> test_sent;
  // The session each title went to join from an accepted invite, watched
  // until XNet reaches its host or gives up on it.
  struct Joining {
    std::array<uint8_t, 8> key{};
    uint64_t inviter = 0;
    std::chrono::steady_clock::time_point at;
    bool failed = false;
  };
  std::array<std::optional<Joining>, XUserMaxUserCount> joining;
  LiveJoinFailedHandler join_failed;
  LiveSystemLinkJoinHandler system_link_join;
  LiveInviteTitleHandler other_title;
  std::chrono::milliseconds join_settle{3000};
};

// How long after a title reads an invite its join is watched.
constexpr auto kJoinWatch = std::chrono::seconds(60);

Invites& GetInvites() {
  static Invites invites;
  return invites;
}

// The inviter's session: from the invite, or else from their presence.
std::vector<uint8_t> InviteSession(uint32_t user_index,
                                   const net::live::Invite& invite) {
  if (invite.info.size() == sizeof(XSESSION_INFO)) {
    return invite.info;
  }
  const auto presence = LiveFriendPresence(user_index, invite.from);
  if (presence && presence->joinable &&
      presence->joinable->info.size() == sizeof(XSESSION_INFO)) {
    return presence->joinable->info;
  }
  return {};
}

constexpr size_t kAnsweredKept = 32;

// The session ID of |info|, an XSESSION_INFO, in hex as presence has it.
std::string SessionHex(const std::vector<uint8_t>& info) {
  if (info.size() != sizeof(XSESSION_INFO)) {
    return {};
  }
  std::string hex;
  for (size_t i = 0; i < 8; ++i) {
    hex += fmt::format("{:02x}", info[i]);
  }
  return hex;
}

// Hands |accepted|, an invite to a game that isn't the running one
// (|running_title|), to the app, which starts that game for it.
void StartInviteTitle(uint32_t user_index, AcceptedInvite accepted,
                      const std::string& room, const std::string& from,
                      uint32_t running_title) {
  LiveInviteTitleHandler handler;
  {
    auto& invites = GetInvites();
    std::lock_guard lock(invites.mutex);
    handler = invites.other_title;
  }
  const std::string running =
      running_title ? fmt::format("{:08X} runs", running_title)
                    : std::string("no game runs");
  if (!handler) {
    XELOGW("Live invite: user {} accepted {}'s invite to title {:08X}, but "
           "{} and nothing here can start that game",
           user_index, from, accepted.title_id, running);
    return;
  }
  XELOGI("Live invite: user {} accepted {}'s invite to title {:08X} while {}; "
         "starting that game for it",
         user_index, from, accepted.title_id, running);
  handler(user_index, from, std::move(accepted), room);
}

// Keeps |accepted| as the session the profile at |user_index| joins and tells
// the title (XN_LIVE_INVITE_ACCEPTED).
// |invite_id|: the pending invite this answers, or empty for a join that
// came without one.
void Deliver(uint32_t user_index, AcceptedInvite accepted,
             const std::string& room, const std::string& from,
             const std::string& invite_id) {
  const uint32_t title_id = accepted.title_id;
  KernelState* kernel_state;
  {
    auto& invites = GetInvites();
    std::lock_guard lock(invites.mutex);
    // Unless a newer invite came meanwhile.
    if (!invite_id.empty() && invites.pending[user_index] &&
        invites.pending[user_index]->id == invite_id) {
      invites.pending[user_index].reset();
    }
    kernel_state = invites.kernel_state;
  }
  const uint32_t running_title = kernel_state ? kernel_state->title_id() : 0;
  if (title_id != running_title) {
    // Another game's invite (or one with no game running): as on the
    // console, that game starts for it and reads the invite as it boots. The
    // app quits the running one and starts it.
    StartInviteTitle(user_index, std::move(accepted), room, from,
                     running_title);
    return;
  }
  {
    auto& invites = GetInvites();
    std::lock_guard lock(invites.mutex);
    invites.accepted[user_index] = std::move(accepted);
    invites.accepted_for[user_index] = kernel_state;
  }
  if (!kernel_state) {
    return;
  }
  // The session's players meet in its System Link room.
  {
    const auto ours = SystemLinkRoomCode(title_id);
    if (ours && *ours != room) {
      JoinSystemLinkRoom(title_id, room);
    }
  }
  XELOGI("Live invite: user {} accepted the invite from {}", user_index, from);
  kernel_state->BroadcastNotification(kXNotificationLiveInviteAccepted,
                                      user_index);
}

// The inviter (or the friend joined) is in a System Link game: a session
// without Live presence, so there's no XSESSION_INFO for the title to join.
// The title's own System Link screen finds that game once both play in the
// same room, so the profile moves to the inviter's room and the app says to
// open it. The title isn't told: it would find no invite to read.
void JoinSystemLinkGame(uint32_t user_index, const net::live::Invite& invite,
                        uint32_t running_title) {
  // The room the inviter's presence shows, which is where they play now;
  // else the invite's.
  std::string room = invite.room;
  const auto presence = LiveFriendPresence(user_index, invite.from);
  if (presence && presence->joinable && presence->title_id == invite.title_id) {
    room = presence->joinable->room;
  }
  LiveSystemLinkJoinHandler handler;
  {
    auto& invites = GetInvites();
    std::lock_guard lock(invites.mutex);
    if (!invite.id.empty() && invites.pending[user_index] &&
        invites.pending[user_index]->id == invite.id) {
      invites.pending[user_index].reset();
    }
    handler = invites.system_link_join;
  }
  if (running_title != invite.title_id) {
    // Another game: it starts in the friend's room, with no session to read.
    AcceptedInvite accepted;
    accepted.inviter = invite.from;
    accepted.title_id = invite.title_id;
    accepted.from_game_invite = !invite.id.empty();
    if (auto account = LiveAccount(user_index)) {
      accepted.invitee = account->xuid();
    }
    StartInviteTitle(user_index, std::move(accepted), room, invite.gamertag,
                     running_title);
    return;
  }
  if (running_title == invite.title_id) {
    const auto ours = SystemLinkRoomCode(invite.title_id);
    if (!ours || *ours != room) {
      JoinSystemLinkRoom(invite.title_id, room);
    }
  }
  XELOGI("Live invite: {} is in a System Link game of {:08X} ({}); user {} "
         "joins it from the game's System Link screen",
         invite.gamertag, invite.title_id,
         room.empty() ? "the public lobby" : "a private room", user_index);
  if (handler) {
    handler(user_index, invite.gamertag, invite.title_id, room);
  }
}

// Accepts |invite| for the profile at |user_index| and tells the title.
bool Accept(uint32_t user_index, const net::live::Invite& invite) {
  auto account = LiveAccount(user_index);
  if (!account) {
    return false;
  }
  AcceptedInvite accepted;
  accepted.invitee = account->xuid();
  accepted.inviter = invite.from;
  accepted.title_id = invite.title_id;
  accepted.info = InviteSession(user_index, invite);
  accepted.from_game_invite = !invite.id.empty();
  uint32_t running_title = 0;
  {
    auto& invites = GetInvites();
    std::lock_guard lock(invites.mutex);
    if (invites.kernel_state) {
      running_title = invites.kernel_state->title_id();
    }
    if (!invite.id.empty()) {
      if (std::find(invites.answered.begin(), invites.answered.end(),
                    invite.id) != invites.answered.end()) {
        XELOGI("Live invite: user {} already accepted invite {} from {}; "
               "not joining again",
               user_index, invite.id, invite.gamertag);
        return true;
      }
      invites.answered.push_back(invite.id);
      if (invites.answered.size() > kAnsweredKept) {
        invites.answered.erase(invites.answered.begin());
      }
    }
  }
  // Already in that game (its session is the one friends see the profile
  // in): joining it again would leave it and come back.
  const std::string session = SessionHex(accepted.info);
  const auto own = LiveOwnJoinable(user_index);
  if (!session.empty() && own && own->session_id == session &&
      running_title == invite.title_id) {
    XELOGI("Live invite: user {} is already in {}'s game (session {})",
           user_index, invite.gamertag, session);
    if (!invite.id.empty()) {
      account->ReplyInvite(invite.id, true);
    }
    return true;
  }
  // Without an ID it's a friend's game the player joins from presence.
  if (!invite.id.empty()) {
    account->ReplyInvite(invite.id, true);
  }
  if (accepted.info.empty()) {
    JoinSystemLinkGame(user_index, invite, running_title);
    return true;
  }
  Deliver(user_index, std::move(accepted), invite.room, invite.gamertag,
          invite.id);
  return true;
}

void OnInvite(uint32_t user_index, const net::live::Invite& invite) {
  if (invite.party) {
    LivePartyOnInvite(user_index, invite);
    return;
  }
  XELOGI("Live invite: from {} to user {}", invite.gamertag, user_index);
  {
    auto& invites = GetInvites();
    std::lock_guard lock(invites.mutex);
    invites.pending[user_index] = invite;  // The newest wins.
  }
  if (cvars::live_auto_accept_invites) {
    Accept(user_index, invite);
    if (cvars::live_test_accept_again) {
      std::thread([user_index, invite] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        XELOGI("Live invite: accepting invite {} again (test)", invite.id);
        Accept(user_index, invite);
      }).detach();
    }
  }
}

}  // namespace

namespace {

// Under |invites.mutex|.
void WatchJoin(Invites& invites, uint32_t user_index,
               const AcceptedInvite& taken) {
  Invites::Joining joining;
  std::memcpy(joining.key.data(), taken.info.data(), joining.key.size());
  joining.inviter = taken.inviter;
  joining.at = std::chrono::steady_clock::now();
  invites.joining[user_index] = joining;
}

// After a join's association failed, and the settle time with no other
// reaching the host: says why, from where the inviter's presence is now.
void ReportJoinFailure(uint32_t user_index, std::array<uint8_t, 8> key) {
  auto& invites = GetInvites();
  uint64_t inviter;
  LiveJoinFailedHandler handler;
  {
    std::lock_guard lock(invites.mutex);
    auto& joining = invites.joining[user_index];
    if (!joining || joining->key != key || !joining->failed) {
      return;
    }
    inviter = joining->inviter;
    joining.reset();
    handler = invites.join_failed;
  }
  const auto presence = LiveFriendPresence(user_index, inviter);
  const bool hosting =
      presence && presence->online && presence->joinable &&
      presence->joinable->info.size() == sizeof(XSESSION_INFO) &&
      std::equal(key.begin(), key.end(), presence->joinable->info.begin());
  const std::string gamertag = presence ? presence->gamertag : std::string();
  const auto reason =
      hosting ? LiveJoinFailure::kHostUnreachable : LiveJoinFailure::kHostLeft;
  XELOGI("Live invite: user {} couldn't join {}'s session {:016x}: {}",
         user_index, gamertag.empty() ? "the inviter" : gamertag,
         xe::load_and_swap<uint64_t>(key.data()),
         hosting ? "the host can't be reached" : "the host left it");
  if (handler) {
    handler(user_index, gamertag, reason);
  }
}

}  // namespace

void LiveJoinAssociation(const std::array<uint8_t, 8>& key, bool connected) {
  auto& invites = GetInvites();
  std::vector<uint32_t> failed;
  std::chrono::milliseconds settle;
  {
    std::lock_guard lock(invites.mutex);
    const auto now = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
      auto& joining = invites.joining[i];
      if (!joining || joining->key != key) {
        continue;
      }
      if (connected || now - joining->at > kJoinWatch) {
        joining.reset();
      } else if (!joining->failed) {
        joining->failed = true;
        failed.push_back(i);
      }
    }
    settle = invites.join_settle;
  }
  // Called under System Link's lock: the presence lookup and the app's
  // handler come later, on their own thread, once a retry or another of the
  // session's peers had time to connect.
  for (const uint32_t user_index : failed) {
    std::thread([user_index, key, settle] {
      std::this_thread::sleep_for(settle);
      ReportJoinFailure(user_index, key);
    }).detach();
  }
}

void SetLiveSystemLinkJoinHandler(LiveSystemLinkJoinHandler handler) {
  auto& invites = GetInvites();
  std::lock_guard lock(invites.mutex);
  invites.system_link_join = std::move(handler);
}

void SetLiveJoinFailedHandler(LiveJoinFailedHandler handler,
                              std::chrono::milliseconds settle) {
  auto& invites = GetInvites();
  std::lock_guard lock(invites.mutex);
  invites.join_failed = std::move(handler);
  invites.join_settle = settle;
}

void InstallLiveInvites(KernelState* kernel_state) {
  auto& invites = GetInvites();
  std::optional<std::string> launch_room;
  uint32_t launch_title = 0;
  {
    std::lock_guard lock(invites.mutex);
    if (invites.kernel_state == kernel_state) {
      return;
    }
    invites.kernel_state = kernel_state;
    // Started for an invite accepted in another title.
    if (!cvars::live_accepted_invite.empty()) {
      auto launch = DecodeAcceptedInvite(cvars::live_accepted_invite);
      cvars::live_accepted_invite.clear();
      if (!launch) {
        XELOGW("Live invite: live_accepted_invite isn't an invite; ignored");
      } else {
        launch_room = std::move(launch->room);
        launch_title = launch->invite.title_id;
        XELOGI("Live invite: started for user {}'s invite from {:016X} to "
               "title {:08X}",
               launch->user_index, launch->invite.inviter, launch_title);
        if (!launch->invite.info.empty()) {
          invites.pending[launch->user_index].reset();
          invites.accepted[launch->user_index] = std::move(launch->invite);
          invites.accepted_for[launch->user_index] = nullptr;
        }
      }
    }
    // The invites accepted before this title started are for it.
    for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
      if (invites.accepted[i] && !invites.accepted_for[i]) {
        invites.accepted_for[i] = kernel_state;
      }
    }
  }
  // The friend's game plays in their System Link room.
  if (launch_room) {
    JoinSystemLinkRoom(launch_title, *launch_room);
  }
  SetLiveInviteHandler(OnInvite);
  net::secure::SetAssociationObserver(LiveJoinAssociation);
}

void SetLiveInviteTitleHandler(LiveInviteTitleHandler handler) {
  auto& invites = GetInvites();
  std::lock_guard lock(invites.mutex);
  invites.other_title = std::move(handler);
}

void ForgetLiveInvites(uint32_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return;
  }
  auto& invites = GetInvites();
  std::lock_guard lock(invites.mutex);
  invites.pending[user_index].reset();
  invites.accepted[user_index].reset();
  invites.accepted_for[user_index] = nullptr;
  invites.test_sent[user_index].clear();
}

void ShutdownLiveInvites() {
  SetLiveInviteHandler(nullptr);
  auto& invites = GetInvites();
  std::lock_guard lock(invites.mutex);
  // An accepted invite this title never read goes with it, so the next
  // title (or this one started again) doesn't join that game. One accepted
  // in the library meanwhile is for the title that starts next, and stays.
  for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
    if (invites.accepted[i] && invites.accepted_for[i] &&
        invites.accepted_for[i] == invites.kernel_state) {
      XELOGI("Live invite: dropping user {}'s accepted invite the title "
             "didn't read",
             i);
      invites.accepted[i].reset();
      invites.accepted_for[i] = nullptr;
    }
  }
  invites.kernel_state = nullptr;
  invites.pending = {};
  invites.test_sent = {};
  invites.joining = {};
}

uint32_t LiveSendInvites(KernelState* kernel_state,
                         const InviteSendRequest& request) {
  auto account = LiveAccount(request.user_index);
  if (!account) {
    XELOGW("Live invite: user {} has no online account, nothing sent",
           request.user_index);
    return 0;
  }
  auto joinable = LiveOwnJoinable(request.user_index);
  if (!joinable) {
    // The invitee looks the session up from presence once there is one.
    XELOGW("Live invite: user {} is in no session friends may join",
           request.user_index);
    joinable.emplace();
  }
  const std::string text = xe::to_utf8(request.text);
  uint32_t sent = 0;
  for (const uint64_t xuid : request.invitees) {
    if (xuid == account->xuid()) {
      continue;
    }
    account->SendInvite(xuid, kernel_state->title_id(), *joinable, text);
    ++sent;
  }
  XELOGI("Live invite: sent by user {} to {} friend(s)", request.user_index,
         sent);
  return sent;
}

std::optional<AcceptedInvite> TakeAcceptedInvite(uint32_t user_index,
                                                 uint32_t title_id) {
  if (user_index >= XUserMaxUserCount) {
    return std::nullopt;
  }
  auto& invites = GetInvites();
  uint64_t inviter;
  {
    std::lock_guard lock(invites.mutex);
    auto& accepted = invites.accepted[user_index];
    // Another game's invite isn't this one's to read.
    if (!accepted || (title_id && accepted->title_id != title_id)) {
      return std::nullopt;
    }
    if (accepted->info.size() == sizeof(XSESSION_INFO)) {
      auto taken = std::move(accepted);
      accepted.reset();
      invites.accepted_for[user_index] = nullptr;
      WatchJoin(invites, user_index, *taken);
      return taken;
    }
    inviter = accepted->inviter;
  }
  // It came without the session, and the inviter's presence had none at the
  // time: it may have one now. Else the invite stays for a later call.
  const auto presence = LiveFriendPresence(user_index, inviter);
  if (!presence || !presence->joinable ||
      presence->joinable->info.size() != sizeof(XSESSION_INFO)) {
    return std::nullopt;
  }
  std::lock_guard lock(invites.mutex);
  auto& accepted = invites.accepted[user_index];
  if (!accepted || accepted->inviter != inviter) {
    return std::nullopt;
  }
  auto taken = std::move(accepted);
  accepted.reset();
  invites.accepted_for[user_index] = nullptr;
  taken->info = presence->joinable->info;
  WatchJoin(invites, user_index, *taken);
  return taken;
}

bool AcceptPendingInvite(uint32_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return false;
  }
  std::optional<net::live::Invite> invite;
  {
    auto& invites = GetInvites();
    std::lock_guard lock(invites.mutex);
    invite = invites.pending[user_index];
  }
  if (!invite) {
    return false;
  }
  Accept(user_index, *invite);
  return true;
}

bool LiveJoinSession(uint32_t user_index, uint64_t from,
                     const std::string& gamertag, uint32_t title_id,
                     const net::live::Joinable& joinable) {
  auto account = LiveAccount(user_index);
  KernelState* kernel_state;
  {
    auto& invites = GetInvites();
    std::lock_guard lock(invites.mutex);
    kernel_state = invites.kernel_state;
  }
  if (!account || !kernel_state || kernel_state->title_id() != title_id ||
      joinable.info.size() != sizeof(XSESSION_INFO)) {
    return false;
  }
  AcceptedInvite accepted;
  accepted.invitee = account->xuid();
  accepted.inviter = from;
  accepted.title_id = title_id;
  accepted.info = joinable.info;
  Deliver(user_index, std::move(accepted), joinable.room, gamertag, {});
  return true;
}

bool AcceptLiveInvite(uint32_t user_index, const net::live::Invite& invite) {
  if (user_index >= XUserMaxUserCount) {
    return false;
  }
  return Accept(user_index, invite);
}

void SetAcceptedInvite(uint32_t user_index, AcceptedInvite invite) {
  if (user_index >= XUserMaxUserCount) {
    return;
  }
  XELOGI("Live invite: user {} accepted {:016X}'s {} for title {:08X} before "
         "it started",
         user_index, invite.inviter,
         invite.from_game_invite ? "invite" : "game", invite.title_id);
  auto& invites = GetInvites();
  std::lock_guard lock(invites.mutex);
  invites.pending[user_index].reset();
  invites.accepted[user_index] = std::move(invite);
  invites.accepted_for[user_index] = nullptr;  // The title that starts next.
}

void ClearAcceptedInvites() {
  auto& invites = GetInvites();
  std::lock_guard lock(invites.mutex);
  for (auto& accepted : invites.accepted) {
    accepted.reset();
  }
  invites.accepted_for = {};
}

std::vector<uint32_t> UsersWithAcceptedInvite(uint32_t title_id) {
  std::vector<uint32_t> users;
  auto& invites = GetInvites();
  std::lock_guard lock(invites.mutex);
  for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
    if (invites.accepted[i] && invites.accepted[i]->title_id == title_id) {
      users.push_back(i);
    }
  }
  return users;
}

std::vector<XSESSION_INFO> LiveKnownSessions() {
  std::vector<std::vector<uint8_t>> infos;
  for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
    for (const auto& peer : LiveFriends(i)) {
      const auto presence = LiveFriendPresence(i, peer.xuid);
      if (presence && presence->joinable) {
        infos.push_back(presence->joinable->info);
      }
    }
  }
  {
    auto& invites = GetInvites();
    std::lock_guard lock(invites.mutex);
    for (const auto& invite : invites.pending) {
      if (invite) {
        infos.push_back(invite->info);
      }
    }
    for (const auto& accepted : invites.accepted) {
      if (accepted) {
        infos.push_back(accepted->info);
      }
    }
  }
  std::vector<XSESSION_INFO> sessions;
  for (const auto& info : infos) {
    if (info.size() != sizeof(XSESSION_INFO)) {
      continue;
    }
    XSESSION_INFO session;
    std::memcpy(&session, info.data(), sizeof(session));
    bool known = false;
    for (const auto& other : sessions) {
      known |= other.session_id == session.session_id;
    }
    if (!known) {
      sessions.push_back(session);
    }
  }
  return sessions;
}

void LiveInvitesPoll(uint32_t user_index) {
  if (cvars::live_test_invite_xuid.empty() || user_index != 0) {
    return;
  }
  const uint64_t xuid =
      std::strtoull(cvars::live_test_invite_xuid.c_str(), nullptr, 16);
  auto account = LiveAccount(user_index);
  const auto joinable = LiveOwnJoinable(user_index);
  if (!xuid || !account || !joinable ||
      joinable->session_id.empty() != cvars::live_test_invite_system_link ||
      !account->IsFriend(xuid)) {
    return;
  }
  const std::string sent_for = joinable->session_id.empty()
                                   ? "room:" + joinable->room
                                   : joinable->session_id;
  KernelState* kernel_state;
  {
    auto& invites = GetInvites();
    std::lock_guard lock(invites.mutex);
    if (invites.test_sent[user_index] == sent_for || !invites.kernel_state) {
      return;
    }
    invites.test_sent[user_index] = sent_for;
    kernel_state = invites.kernel_state;
  }
  const uint32_t title_id = kernel_state->title_id();
  account->SendInvite(xuid, title_id, *joinable, "");
  XELOGI("Live invite: sent (test) to {:016X} for {}", xuid,
         joinable->session_id.empty() ? "a System Link game"
                                      : "session " + joinable->session_id);
  if (cvars::live_test_invite_again_s > 0) {
    std::thread([user_index, xuid, title_id,
                 delay = cvars::live_test_invite_again_s] {
      std::this_thread::sleep_for(std::chrono::seconds(delay));
      auto account = LiveAccount(user_index);
      const auto joinable = LiveOwnJoinable(user_index);
      if (!account || !joinable) {
        return;
      }
      account->SendInvite(xuid, title_id, *joinable, "");
      XELOGI("Live invite: sent (test, again) to {:016X} for session {}", xuid,
             joinable->session_id);
    }).detach();
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
