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
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xenumerator.h"

#include <algorithm>
#include <cstring>

#include <asio.hpp>

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

struct X_CREATE_FRIENDS_ENUMERATOR {
  X_ARGUMENT_ENTRY user_index;
  X_ARGUMENT_ENTRY friends_starting_index;
  X_ARGUMENT_ENTRY friends_amount;
  X_ARGUMENT_ENTRY buffer_size_ptr;
  X_ARGUMENT_ENTRY handle_ptr;
};

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

constexpr uint32_t kOnlineMaxFriends = 100;
constexpr uint32_t kOnlineFriendSize = 0xC4;  // X_ONLINE_FRIEND

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
      // buffer_length holds the address of the BOOL result. No mute list is
      // kept so nobody is muted.
      if (buffer_ptr) {
        xe::store_and_swap<uint32_t>(buffer + 0x10, X_ERROR_SUCCESS);
      }
      if (buffer_length) {
        xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(buffer_length),
                                     0);
      }
      return X_E_SUCCESS;
    }
    case 0x00058020: {
      XELOGD("XFriendsCreateEnumerator({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return XFriendsCreateEnumerator(buffer_length);
    }
    case 0x00058023: {
      XELOGD(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
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
  std::memset(results, 0, results_size);  // X_E_SUCCESS for every string.
  xe::store_and_swap<uint16_t>(results, count);
  xe::store_and_swap<uint32_t>(results + 2, results_ptr + kResponseHeaderSize);
  XELOGD("XStringVerify: {} strings", count);
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

  // No friends list is kept so the enumerator is always empty.
  auto e = make_object<XStaticUntypedEnumerator>(kernel_state_, amount,
                                                 kOnlineFriendSize);
  auto result = e->Initialize(user_index, app_id(), 0x58021, 0x58022, 0);
  if (XFAILED(result)) {
    return result;
  }

  xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(buffer_size_ptr),
                               amount * kOnlineFriendSize);
  xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(handle_ptr),
                               e->handle());
  return X_E_SUCCESS;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
