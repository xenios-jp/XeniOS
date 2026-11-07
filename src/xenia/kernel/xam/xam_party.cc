/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_live_invites.h"
#include "xenia/kernel/xam/xam_live_party.h"
#include "xenia/kernel/xam/xam_live_social.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// XamPartyPartyCreate, XamPartyLeave, XamPartySendInvite, XamPartyJoinEx, &
// XamPartyRemoveLocalUsers share one function

namespace {

// The party of the first profile in one, as XamPartyGetUserList(Internal)
// write it; false when no profile is in a party.
template <typename List, typename Fill>
bool FillPartyList(List* list, Fill&& fill) {
  const auto user_index = LivePartyUser();
  if (!user_index) {
    return false;
  }
  auto account = LiveAccount(*user_index);
  auto party = LiveParty(*user_index);
  if (!account || !party) {
    return false;
  }
  const auto own = LiveOwnJoinable(*user_index);
  const std::vector<uint8_t> own_session =
      own ? own->info : std::vector<uint8_t>();
  const uint32_t count =
      std::min<uint32_t>(uint32_t(party->members.size()), kPartyMaxUsers);
  list->user_count = count;
  for (uint32_t i = 0; i < count; ++i) {
    fill(*party, i, account->xuid(), *user_index, own_session,
         &list->users[i]);
  }
  return true;
}

}  // namespace

dword_result_t XamPartyGetUserList_entry(dword_t caller,
                                         pointer_t<X_PARTY_USER_LIST> list) {
  if (caller != 1) {
    return X_E_NOT_IMPLEMENTED;
  }
  // Titles read the count without checking the result.
  if (!list) {
    return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
  }
  list.Zero();
  if (FillPartyList(static_cast<X_PARTY_USER_LIST*>(list),
                    [](const auto& party, size_t i, uint64_t xuid,
                       uint32_t index, const auto& own,
                       X_PARTY_USER_INFO* out) {
                      FillPartyUser(party, i, xuid, index, own, out);
                    })) {
    return X_ERROR_SUCCESS;
  }
  // 5345085D, 45410923
  return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
}
DECLARE_XAM_EXPORT1(XamPartyGetUserList, kNone, kImplemented);

dword_result_t XamPartyGetUserListInternal_entry(
    pointer_t<X_PARTY_USER_LIST_INTERNAL> list) {
  if (!list) {
    return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
  }
  list.Zero();
  if (FillPartyList(static_cast<X_PARTY_USER_LIST_INTERNAL*>(list),
                    [](const auto& party, size_t i, uint64_t xuid,
                       uint32_t index, const auto& own,
                       X_PARTY_USER_INFO_INTERNAL* out) {
                      FillPartyUser(party, i, xuid, index, own, &out->info);
                    })) {
    return X_ERROR_SUCCESS;
  }
  // FFFE07D1
  return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
}
DECLARE_XAM_EXPORT1(XamPartyGetUserListInternal, kNone, kImplemented);

// Shares same function as XamPartyAddLocalUsers, XamPartyKickUser,
// XamPartyGetJoinable, XamPartySetJoinable, & XamPartyGetInfoEx
// Sends a game invite to the session the profile is in to every other party
// member.
dword_result_t XamPartySendGameInvites_entry(
    dword_t caller, dword_t user_index, dword_t unknown,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  const uint32_t user = user_index;
  auto run = [user](uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;
    auto party = LiveParty(user);
    auto account = LiveAccount(user);
    if (!party || !account) {
      extended_error = 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
      return X_ERROR_FUNCTION_FAILED;
    }
    InviteSendRequest request;
    request.user_index = user;
    for (const auto& member : party->members) {
      if (member.first != account->xuid()) {
        request.invitees.push_back(member.first);
      }
    }
    LiveSendInvites(kernel_state(), request);
    return X_ERROR_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    X_RESULT result = run(extended_error, length);

    return result;
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamPartySendGameInvites, kNone, kImplemented);

dword_result_t XamPartySetCustomData_entry(dword_t caller, dword_t user_index,
                                           lpvoid_t custom_data_ptr) {
  if (caller != 1) {
    return X_E_NOT_IMPLEMENTED;
  } else if (user_index >= XUserMaxUserCount) {
    return 0x807D0002;
  }
  return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
}
DECLARE_XAM_EXPORT1(XamPartySetCustomData, kNone, kStub);

dword_result_t XamPartyGetBandwidth_entry(dword_t bandwidth_type,
                                          lpqword_t bandwidth_statstic) {
  if (bandwidth_statstic) {
    *bandwidth_statstic = 0;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamPartyGetBandwidth, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Party);
