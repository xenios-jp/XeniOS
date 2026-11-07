/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xlivebase_app.h"

#include "xenia/base/logging.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/base/string.h"
#include "xenia/kernel/util/xfiletime.h"
#include "xenia/kernel/xam/xam_live_invites.h"
#include "xenia/kernel/xam/xam_live_social.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xenumerator.h"

#include <algorithm>
#include <cstring>

#include <asio.hpp>

#include "third_party/fmt/include/fmt/ranges.h"

struct XONLINE_SERVICE_INFO {
  xe::be<uint32_t> id;
  in_addr ip;
  xe::be<uint16_t> port;
  xe::be<uint16_t> reserved;
};

struct X_ARGUMENT_ENTRY {
  xe::be<uint32_t> native_size;
  xe::be<uint64_t> argument_value_ptr;
};
static_assert_size(X_ARGUMENT_ENTRY, 0x10);

// XUserMuteListQuery's arguments, laid out as the console's compiler does.
struct X_MUTE_LIST_QUERY {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> result;
};
static_assert_size(X_MUTE_LIST_QUERY, 0x18);

struct X_CREATE_FRIENDS_ENUMERATOR {
  X_ARGUMENT_ENTRY user_index;
  X_ARGUMENT_ENTRY friends_starting_index;
  X_ARGUMENT_ENTRY friends_amount;
  X_ARGUMENT_ENTRY buffer_size_ptr;
  X_ARGUMENT_ENTRY handle_ptr;
};

// Layouts from the Xenia Netplay fork.
struct X_PRESENCE_CREATE {
  X_ARGUMENT_ENTRY user_index;
  X_ARGUMENT_ENTRY num_peers;
  X_ARGUMENT_ENTRY peer_xuids_ptr;
  X_ARGUMENT_ENTRY starting_index;
  X_ARGUMENT_ENTRY max_peers;
  X_ARGUMENT_ENTRY buffer_length_ptr;
  X_ARGUMENT_ENTRY enumerator_handle_ptr;
};

struct X_PRESENCE_GET_STATE {
  X_ARGUMENT_ENTRY xuid;
  X_ARGUMENT_ENTRY state_flags_ptr;
  X_ARGUMENT_ENTRY session_id_ptr;
};
static_assert_size(X_PRESENCE_GET_STATE, 0x30);

constexpr uint32_t kRichPresenceSize = 64;

// 4-byte aligned on the console.
#pragma pack(push, 4)
struct X_ONLINE_PRESENCE {
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> state;
  uint8_t session_id[8];
  xe::be<uint32_t> title_id;
  xe::kernel::X_FILETIME state_change_time;
  xe::be<uint32_t> rich_presence_size;
  xe::be<char16_t> rich_presence[kRichPresenceSize];
};
static_assert_size(X_ONLINE_PRESENCE, 0xA4);

struct X_ONLINE_FRIEND {
  xe::be<uint64_t> xuid;
  char gamertag[16];
  xe::be<uint32_t> state;
  uint8_t session_id[8];
  xe::be<uint32_t> title_id;
  xe::kernel::X_FILETIME user_time;
  uint8_t invite_session_id[8];
  xe::kernel::X_FILETIME game_invite_time;
  xe::be<uint32_t> rich_presence_size;
  xe::be<char16_t> rich_presence[kRichPresenceSize];
};
#pragma pack(pop)
static_assert_size(X_ONLINE_FRIEND, 0xC4);

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

constexpr uint32_t kOnlineMaxFriends = 100;
constexpr uint32_t kOnlineFriendSize = sizeof(X_ONLINE_FRIEND);

// X_ONLINE_FRIENDSTATE flags.
constexpr uint32_t kFriendOnline = 0x00000001;
constexpr uint32_t kFriendPlaying = 0x00000002;
constexpr uint32_t kFriendJoinable = 0x00000010;
constexpr uint32_t kFriendXbox360 = 0x00001000;
constexpr uint32_t kFriendSentRequest = 0x40000000;
constexpr uint32_t kFriendReceivedRequest = 0x80000000;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state)
    : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

namespace {

// An asynchronous Live call. Its message ID is the function's index in the
// online schema the title was built with, which differs between XDK versions,
// so these are told apart by the function's ordinal instead.
struct X_XLIVE_ASYNC_TASK {
  xe::be<uint32_t> ordinal;
  xe::be<uint32_t> schema_data_ptr;
  xe::be<uint32_t> schema_index;
  xe::be<uint32_t> task_flags;
  xe::be<uint32_t> internal_task_ptr;
  xe::be<uint32_t> internal_task_size;
  xe::be<uint32_t> marshalled_request_ptr;
  xe::be<uint32_t> marshalled_request_size;
  xe::be<uint32_t> total_wire_buffer_size;
  xe::be<uint32_t> counter;
  xe::be<uint32_t> logon_id;
  xe::be<uint32_t> results_ptr;
  xe::be<uint32_t> results_size;
  uint8_t wire_buffer[0x14];
  xe::be<uint32_t> overlapped_ptr;
};
static_assert_size(X_XLIVE_ASYNC_TASK, 0x4C);

// The message argument: the task, then the progress counters.
constexpr uint32_t kAsyncMessageSize = 0x28;

// A friend's state flags and joinable session, from their presence.
uint32_t PresenceState(const std::optional<net::live::Presence>& presence,
                       uint8_t session_id[8]) {
  std::memset(session_id, 0, 8);
  if (!presence || !presence->online) {
    return 0;
  }
  uint32_t state = kFriendOnline | kFriendXbox360;
  if (presence->title_id) {
    state |= kFriendPlaying;
  }
  if (presence->joinable && presence->joinable->session_id.size() == 16) {
    state |= kFriendJoinable;
    for (size_t i = 0; i < 8; ++i) {
      session_id[i] = static_cast<uint8_t>(std::strtoul(
          presence->joinable->session_id.substr(i * 2, 2).c_str(), nullptr,
          16));
    }
  }
  return state;
}

template <typename T>
void SetRichPresence(T* item, const std::optional<net::live::Presence>& p) {
  const std::u16string rich =
      p ? xe::to_utf16(p->rich).substr(0, kRichPresenceSize - 1)
        : std::u16string();
  for (size_t i = 0; i < rich.size(); ++i) {
    item->rich_presence[i] = rich[i];
  }
  item->rich_presence_size =
      rich.empty() ? 0 : static_cast<uint32_t>((rich.size() + 1) * 2);
}

bool IsAsyncMessage(uint32_t message) {
  return (message & 0xFFFF8000) == 0x00050000;
}

// Ordinals of the asynchronous Live functions (from the Xenia Netplay fork).
const char* AsyncOrdinalName(uint32_t ordinal) {
  switch (ordinal) {
    case 0x0305: return "XOnlineQuerySearch";
    case 0x0583: return "XInviteSend";
    case 0x0602: return "XAccountGetPointsBalance";
    case 0x0604: return "XAccountGetUserInfo";
    case 0x0613: return "XPassportGetMemberName";
    case 0x0704: return "XUserEstimateRankForRating";
    case 0x0716: return "XUserValidateAvatarManifest";
    case 0x0801: return "XStringVerify";
    case 0x0900: return "XOfferingContentEnumerate";
    case 0x0904: return "XOfferingSubscriptionEnumerate";
    case 0x0908: return "XBannerGetList";
    case 0x0909: return "XBannerGetListHot";
    case 0x0A03: return "XUserFindUsers";
    case 0x0E11: return "XStorageDownloadToMemory";
    case 0x0E13: return "XStorageUploadFromMemory";
    case 0x0E14: return "XStorageEnumerate";
    case 0x0E15: return "XStorageDelete";
    default: return "unknown";
  }
}

// Hex of an unknown message's arguments, and of the block its first word
// points to (most messages point to an XLIVE_ASYNC_TASK), to tell what the
// title asks for.
void LogMessageArgs(Memory* memory, uint32_t message, uint32_t buffer_ptr,
                    uint32_t buffer_length) {
  if (!buffer_ptr || !buffer_length) {
    return;
  }
  auto hex = [](const uint8_t* bytes, uint32_t size) {
    std::string text;
    for (uint32_t i = 0; i < size; ++i) {
      text += fmt::format("{:02X}{}", bytes[i], (i % 4 == 3) ? " " : "");
    }
    return text;
  };
  const auto* args = memory->TranslateVirtual<const uint8_t*>(buffer_ptr);
  XELOGD("  XLIVEBASE {:08X} args: {}", message,
         hex(args, std::min<uint32_t>(buffer_length, 0x40)));
  const uint32_t inner = xe::load_and_swap<uint32_t>(args);
  if (inner >= 0x10000 && memory->LookupHeap(inner)) {
    XELOGD("  XLIVEBASE {:08X} *args: {}", message,
           hex(memory->TranslateVirtual<const uint8_t*>(inner), 0x80));
  }
}

}  // namespace

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message,
                                            uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  if (IsAsyncMessage(message) && buffer_ptr &&
      buffer_length == kAsyncMessageSize) {
    const uint32_t task_ptr = xe::load_and_swap<uint32_t>(buffer);
    if (task_ptr && memory_->LookupHeap(task_ptr)) {
      return DispatchAsyncTask(message, buffer_ptr, task_ptr);
    }
  }
  switch (message) {
    case 0x0005008C: {
      // Called on startup of blades dashboard v1888 to v2858
      XELOGD("XLiveBaseUnk5008C, unimplemented");
      return X_E_FAIL;
    }
    case 0x00050094: {
      // Called on startup of blades dashboard v4532 to v4552
      XELOGD("XLiveBaseUnk50094, unimplemented");
      return X_E_FAIL;
    }
    case 0x00058003: {
      /* Notes:
         - Called on startup of dashboard (netplay build)
         - used by other internet funtions to check if online (e.g.
         XamGetLiveHiveValueA)
         - Return is Saved elsewhere and used here
      */
      XELOGD("XLiveBaseLogonGetHR");
      return AnyUserSignedInToLive()
                 ? 0x001510F0   // X_ONLINE_S_LOGON_CONNECTION_ESTABLISHED
                 : 0x001510F1;  // X_ONLINE_S_LOGON_DISCONNECTED
    }
    case 0x00058004: {
      /* Notes:
         - Called on startup, seems to just return a bool in the buffer.
         - It is Saved elsewhere and used here
      */
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      xe::store_and_swap<uint32_t>(buffer, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      // Buffer only set when online
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetNatType({:08X})", buffer_ptr);
      if (!AnyUserSignedInToLive()) {
        return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
      }
      // Peers reach each other through the System Link relay whatever the
      // host's NAT, so report XONLINE_NAT_OPEN.
      if (buffer_ptr) {
        xe::store_and_swap<uint32_t>(buffer, 1);
      }
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      XELOGD("XLiveBaseOnlineGetServiceInfo({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x0005800E: {
      // XUserMuteListQuery. The buffer ends in the result code and
      // buffer_length holds the address of the BOOL result. Whoever the
      // profile blocks on the online service is muted.
      bool muted = false;
      if (buffer_ptr) {
        auto* query = memory_->TranslateVirtual<X_MUTE_LIST_QUERY*>(buffer_ptr);
        muted = LiveIsBlocked(query->user_index, query->xuid);
        // TODO(xenios-jp): the argument layout is inferred; check it with a
        // title that queries the mute list.
        XELOGD("XUserMuteListQuery({}, {:016X}) = {}",
               uint32_t(query->user_index), uint64_t(query->xuid), muted);
        query->result = X_ERROR_SUCCESS;
      }
      if (buffer_length) {
        xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(buffer_length),
                                     muted ? 1 : 0);
      }
      return X_E_SUCCESS;
    }
    case 0x00058019: {
      XELOGD("XPresenceCreateEnumerator({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return XPresenceCreateEnumerator(buffer_length);
    }
    case 0x0005801C: {
      XELOGD("XPresenceGetState({:08X}, {:08X})", buffer_ptr, buffer_length);
      return XPresenceGetState(buffer_length);
    }
    case 0x0005801E:
    case 0x0005801F:
    case 0x00058044: {
      // XPresenceSubscribe and XPresenceUnsubscribe. Friends' presence comes
      // without subscribing, and nobody else's is known.
      XELOGD("XPresence(Un)Subscribe({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058056: {
      XELOGD("XPresenceInitializeEx({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058020: {
      XELOGD("XFriendsCreateEnumerator({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return XFriendsCreateEnumerator(buffer_length);
    }
    case 0x00058023: {
      return XInviteGetAcceptedInfo(buffer_length);
    }
    case 0x00058024: {
      // A Live service request. The 17559 dashboard polls its unread message
      // count with one every few seconds.
      XELOGD("XLiveBaseServiceRequest({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x00058037: {
      XELOGD("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      XELOGD("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058017: {
      // GetNextSequenceMessage: the next presence message's sequence number
      // and length. There are none, so answer with an empty message header
      // (0x20 bytes), as the Netplay fork does.
      XELOGD("GetNextSequenceMessage({:08X})", buffer_ptr);
      if (!buffer_ptr) {
        return X_E_INVALIDARG;
      }
      auto* reply = memory_->TranslateVirtual<xe::be<uint32_t>*>(buffer_ptr);
      reply[0] = 0;     // seq_num
      reply[1] = 0x20;  // msg_length, sizeof(BASE_MSG_HEADER)
      return X_E_SUCCESS;
    }
    case 0x00058035: {
      // Fixes Xbox Live error for 513107D9
      // Required for 534507D4
      XELOGD("XLiveBaseUnk58035({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
  }
  // TODO(live): 0x00058009 (args 0, title ID, FFFFFFFF, a pointer) comes
  // once at boot in Gears of War and Virtua Fighter 5; both play online
  // without it.
  XELOGE(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  LogMessageArgs(memory_, message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

X_HRESULT XLiveBaseApp::DispatchAsyncTask(uint32_t message,
                                          uint32_t buffer_ptr,
                                          uint32_t task_ptr) {
  const auto* task =
      memory_->TranslateVirtual<const X_XLIVE_ASYNC_TASK*>(task_ptr);
  const uint32_t ordinal = task->ordinal;
  switch (ordinal) {
    case 0x0305: {  // XOnlineQuerySearch
      XELOGD("XOnlineQuerySearch({:08X}) unimplemented", buffer_ptr);
      return X_E_SUCCESS;
    }
    case 0x0583: {
      return XInviteSend(task_ptr);
    }
    case 0x0801: {
      return XStringVerify(buffer_ptr);
    }
    case 0x0E11: {
      // XStorageDownloadToMemory. There is no storage server so no file is
      // ever found. Success would leave the title's buffer unfilled and
      // 434D083D parses that as its server setup file.
      XELOGD("XStorageDownloadToMemory({:08X})", buffer_ptr);
      return 0x8015C004;  // X_ONLINE_E_STORAGE_FILE_NOT_FOUND
    }
  }
  XELOGE("Unimplemented XLIVEBASE task {} (ordinal {:04X}, msg={:08X})",
         AsyncOrdinalName(ordinal), ordinal, message);
  LogMessageArgs(memory_, message, buffer_ptr, kAsyncMessageSize);
  return X_E_FAIL;
}

// XStringVerify, Live's check that text a player typed is acceptable. Every
// string passes: there is no filter service. The message points to the async
// task, whose marshalled request starts with the title ID, flags, the locale
// length and the string count, and whose results are a STRING_VERIFY_RESPONSE
// followed by one HRESULT per string.
X_HRESULT XLiveBaseApp::XStringVerify(uint32_t buffer_ptr) {
  if (!buffer_ptr) {
    return X_E_INVALIDARG;
  }
  const uint32_t task_ptr = xe::load_and_swap<uint32_t>(
      memory_->TranslateVirtual<const uint8_t*>(buffer_ptr));
  if (!task_ptr) {
    return X_E_INVALIDARG;
  }
  const auto* task = memory_->TranslateVirtual<const xe::be<uint32_t>*>(task_ptr);
  const uint32_t request_ptr = task[6];
  const uint32_t request_size = task[7];
  const uint32_t results_ptr = task[11];
  const uint32_t results_size = task[12];
  constexpr uint32_t kResponseHeaderSize = 6;  // be16 count, be32 pointer.
  if (!request_ptr || request_size < 12 || !results_ptr ||
      results_size < kResponseHeaderSize) {
    return X_E_INVALIDARG;
  }
  const uint16_t requested = xe::load_and_swap<uint16_t>(
      memory_->TranslateVirtual<const uint8_t*>(request_ptr) + 10);
  const uint16_t count = static_cast<uint16_t>(std::min<uint32_t>(
      requested, (results_size - kResponseHeaderSize) / sizeof(uint32_t)));
  auto* results = memory_->TranslateVirtual<uint8_t*>(results_ptr);
  // X_E_SUCCESS for every string; only what's written.
  std::memset(results, 0, kResponseHeaderSize + count * sizeof(uint32_t));
  xe::store_and_swap<uint16_t>(results, count);
  xe::store_and_swap<uint32_t>(results + 2, results_ptr + kResponseHeaderSize);
  XELOGD("XStringVerify: {} strings", count);
  return X_E_SUCCESS;
}

// XInviteSend: an invite to the title's session for each friend listed. The
// task's marshalled request (task words 6 and 7) holds the user, the
// invitees and the text (ParseInviteSendRequest).
X_HRESULT XLiveBaseApp::XInviteSend(uint32_t task_ptr) {
  const auto* task =
      memory_->TranslateVirtual<const xe::be<uint32_t>*>(task_ptr);
  const uint32_t request_ptr = task[6];
  const uint32_t request_size = task[7];
  if (!request_ptr || !request_size || request_size > 0x10000) {
    return X_E_INVALIDARG;
  }
  const auto request = ParseInviteSendRequest(
      {memory_->TranslateVirtual<const uint8_t*>(request_ptr), request_size});
  if (!request || request->user_index >= XUserMaxUserCount) {
    XELOGW("XInviteSend: a request of {} bytes that doesn't parse",
           request_size);
    return X_E_INVALIDARG;
  }
  XELOGI("XInviteSend(user {}, {} invitee(s))", request->user_index,
         request->invitees.size());
  LiveSendInvites(kernel_state_, *request);
  return X_E_SUCCESS;
}

// XInviteGetAcceptedInfo: the invite the profile accepted, for the title to
// join its session. The arguments are an X_ARGUMENT_ENTRY list: the user
// index, then the X_INVITE_INFO to fill.
X_HRESULT XLiveBaseApp::XInviteGetAcceptedInfo(uint32_t args_ptr) {
  if (!args_ptr) {
    return X_E_INVALIDARG;
  }
  const auto* args = memory_->TranslateVirtual<const X_ARGUMENT_ENTRY*>(args_ptr);
  const uint32_t user_index_ptr =
      static_cast<uint32_t>(args[0].argument_value_ptr);
  const uint32_t info_ptr = static_cast<uint32_t>(args[1].argument_value_ptr);
  if (!user_index_ptr || !info_ptr) {
    return X_E_INVALIDARG;
  }
  const uint32_t user_index =
      xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(user_index_ptr));
  if (user_index >= XUserMaxUserCount) {
    return X_E_INVALIDARG;
  }
  auto* info = memory_->TranslateVirtual<X_INVITE_INFO*>(info_ptr);
  const auto accepted =
      TakeAcceptedInvite(user_index, kernel_state_->title_id());
  if (!accepted || !FillInviteInfo(*accepted, info)) {
    std::memset(info, 0, sizeof(*info));
    XELOGI("XInviteGetAcceptedInfo(user {}): none accepted", user_index);
    return 0x80155200;  // X_ONLINE_E_SESSION_NOT_FOUND
  }
  XELOGI(
      "XInviteGetAcceptedInfo(user {}): from {:016X}, title {:08X}, session "
      "{:02x}",
      user_index, accepted->inviter, accepted->title_id,
      fmt::join(info->host_info.session_id, ""));
  return X_E_SUCCESS;
}

X_HRESULT XLiveBaseApp::XFriendsCreateEnumerator(uint32_t args_ptr) {
  if (!args_ptr) {
    return X_E_INVALIDARG;
  }
  const auto args =
      memory_->TranslateVirtual<X_CREATE_FRIENDS_ENUMERATOR*>(args_ptr);
  const uint32_t user_index_ptr =
      static_cast<uint32_t>(args->user_index.argument_value_ptr);
  const uint32_t starting_index_ptr =
      static_cast<uint32_t>(args->friends_starting_index.argument_value_ptr);
  const uint32_t amount_ptr =
      static_cast<uint32_t>(args->friends_amount.argument_value_ptr);
  const uint32_t buffer_size_ptr =
      static_cast<uint32_t>(args->buffer_size_ptr.argument_value_ptr);
  const uint32_t handle_ptr =
      static_cast<uint32_t>(args->handle_ptr.argument_value_ptr);
  if (!user_index_ptr || !starting_index_ptr || !amount_ptr ||
      !buffer_size_ptr || !handle_ptr) {
    return X_E_INVALIDARG;
  }

  const uint32_t user_index =
      xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(user_index_ptr));
  const uint32_t starting_index = xe::load_and_swap<uint32_t>(
      memory_->TranslateVirtual(starting_index_ptr));
  const uint32_t amount =
      xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(amount_ptr));
  if (user_index >= XUserMaxUserCount || starting_index >= kOnlineMaxFriends ||
      amount > kOnlineMaxFriends) {
    return X_E_INVALIDARG;
  }
  if (!kernel_state_->xam_state()->IsUserSignedIn(user_index)) {
    return X_E_NO_SUCH_USER;
  }

  // The profile's friends and friend requests on the online service.
  auto e = make_object<XStaticEnumerator<X_ONLINE_FRIEND>>(kernel_state_,
                                                           amount);
  auto result = e->Initialize(user_index, app_id(), 0x58021, 0x58022, 0);
  if (XFAILED(result)) {
    return result;
  }
  std::vector<net::live::Friend> friends;
  if (auto account = LiveAccount(user_index)) {
    friends = account->Friends();
  }
  for (size_t i = starting_index;
       i < friends.size() && e->item_count() < amount; ++i) {
    const auto& peer = friends[i];
    auto* item = e->AppendItem();
    std::memset(item, 0, sizeof(*item));
    item->xuid = peer.xuid;
    std::strncpy(item->gamertag, peer.gamertag.c_str(),
                 sizeof(item->gamertag) - 1);
    switch (peer.state) {
      case net::live::Friend::State::kFriend: {
        const auto presence = LiveFriendPresence(user_index, peer.xuid);
        item->state = PresenceState(presence, item->session_id);
        item->title_id = presence ? presence->title_id : 0;
        SetRichPresence(item, presence);
        break;
      }
      case net::live::Friend::State::kIncoming:
        item->state = kFriendReceivedRequest;
        break;
      case net::live::Friend::State::kOutgoing:
        item->state = kFriendSentRequest;
        break;
    }
  }
  XELOGD("XFriendsCreateEnumerator: {} of {} friends", e->item_count(),
         friends.size());

  xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(buffer_size_ptr),
                               amount * kOnlineFriendSize);
  xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(handle_ptr),
                               e->handle());
  return X_E_SUCCESS;
}

X_HRESULT XLiveBaseApp::XPresenceCreateEnumerator(uint32_t args_ptr) {
  if (!args_ptr) {
    return X_E_INVALIDARG;
  }
  const auto* args = memory_->TranslateVirtual<X_PRESENCE_CREATE*>(args_ptr);
  auto value = [this](const X_ARGUMENT_ENTRY& entry) {
    return xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(
        static_cast<uint32_t>(entry.argument_value_ptr)));
  };
  const uint32_t xuids_ptr =
      static_cast<uint32_t>(args->peer_xuids_ptr.argument_value_ptr);
  const uint32_t buffer_size_ptr =
      static_cast<uint32_t>(args->buffer_length_ptr.argument_value_ptr);
  const uint32_t handle_ptr =
      static_cast<uint32_t>(args->enumerator_handle_ptr.argument_value_ptr);
  if (!args->user_index.argument_value_ptr || !args->num_peers.argument_value_ptr ||
      !args->starting_index.argument_value_ptr ||
      !args->max_peers.argument_value_ptr || !xuids_ptr || !buffer_size_ptr ||
      !handle_ptr) {
    return X_E_INVALIDARG;
  }
  // Titles expect a handle of 0 on failure (from the Netplay fork).
  xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(handle_ptr), 0);
  xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(buffer_size_ptr), 0);
  const uint32_t user_index = value(args->user_index);
  const uint32_t num_peers = value(args->num_peers);
  const uint32_t starting_index = value(args->starting_index);
  const uint32_t max_peers = value(args->max_peers);
  if (user_index >= XUserMaxUserCount || !num_peers ||
      num_peers > kOnlineMaxFriends || starting_index > num_peers ||
      !max_peers || max_peers > kOnlineMaxFriends) {
    return X_E_INVALIDARG;
  }
  if (!kernel_state_->xam_state()->IsUserSignedIn(user_index)) {
    return X_E_NO_SUCH_USER;
  }
  auto e = make_object<XStaticEnumerator<X_ONLINE_PRESENCE>>(kernel_state_,
                                                             max_peers);
  auto result = e->Initialize(user_index, app_id(), 0x5801A, 0x5801B, 0);
  if (XFAILED(result)) {
    return result;
  }
  const auto* xuids =
      memory_->TranslateVirtual<const xe::be<uint64_t>*>(xuids_ptr);
  for (uint32_t i = starting_index; i < num_peers; ++i) {
    const uint64_t xuid = xuids[i];
    if (!LiveAreFriends(user_index, xuid)) {
      continue;  // Only friends' presence is known.
    }
    const auto presence = LiveFriendPresence(user_index, xuid);
    auto* item = e->AppendItem();
    std::memset(item, 0, sizeof(*item));
    item->xuid = xuid;
    item->state = PresenceState(presence, item->session_id);
    item->title_id = presence ? presence->title_id : 0;
    SetRichPresence(item, presence);
  }
  XELOGD("XPresenceCreateEnumerator: {} of {} peers", e->item_count(),
         num_peers - starting_index);
  xe::store_and_swap<uint32_t>(
      memory_->TranslateVirtual(buffer_size_ptr),
      static_cast<uint32_t>(max_peers * sizeof(X_ONLINE_PRESENCE)));
  xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(handle_ptr),
                               e->handle());
  return X_E_SUCCESS;
}

X_HRESULT XLiveBaseApp::XPresenceGetState(uint32_t args_ptr) {
  if (!args_ptr) {
    return X_E_INVALIDARG;
  }
  const auto* args =
      memory_->TranslateVirtual<X_PRESENCE_GET_STATE*>(args_ptr);
  const uint32_t xuid_ptr = static_cast<uint32_t>(args->xuid.argument_value_ptr);
  const uint32_t state_ptr =
      static_cast<uint32_t>(args->state_flags_ptr.argument_value_ptr);
  const uint32_t session_ptr =
      static_cast<uint32_t>(args->session_id_ptr.argument_value_ptr);
  if (!xuid_ptr || !state_ptr || !session_ptr) {
    return X_E_INVALIDARG;
  }
  const uint64_t xuid =
      xe::load_and_swap<uint64_t>(memory_->TranslateVirtual(xuid_ptr));
  std::optional<net::live::Presence> presence;
  for (uint32_t i = 0; i < XUserMaxUserCount && !presence; ++i) {
    presence = LiveFriendPresence(i, xuid);
  }
  auto* session_id = memory_->TranslateVirtual<uint8_t*>(session_ptr);
  xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(state_ptr),
                               PresenceState(presence, session_id));
  return X_E_SUCCESS;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
