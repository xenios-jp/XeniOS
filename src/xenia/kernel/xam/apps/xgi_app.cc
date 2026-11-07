/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xgi_app.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xam_live_sessions.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xsession.h"

#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/string_util.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {
/*
 * Most of the structs below were found in the Source SDK, provided as stubs.
 * Specifically, they can be found in the Source 2007 SDK and the Alien Swarm
 * Source SDK. Both are available on Steam for free. A GitHub mirror of the
 * Alien Swarm SDK can be found here:
 * https://github.com/NicolasDe/AlienSwarm/blob/master/src/common/xbox/xboxstubs.h
 */

struct X_USER_ACHIEVEMENT {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> achievement_id;
};
static_assert_size(X_USER_ACHIEVEMENT, 0x8);

struct XGI_WRITEACHIEVEMENT {
  xe::be<uint32_t> num_achievements;
  xe::be<uint32_t> achievements_ptr;  // X_USER_ACHIEVEMENT*
};
static_assert_size(XGI_WRITEACHIEVEMENT, 0x8);

struct X_USER_AVATAR_ASSET {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> award_id;
};
static_assert_size(X_USER_AVATAR_ASSET, 0x8);

struct XGI_AWARD_AVATAR_ASSETS {
  xe::be<uint32_t> num_assets;
  xe::be<uint32_t> assets_ptr;  // X_USER_AVATAR_ASSET*
};
static_assert_size(XGI_AWARD_AVATAR_ASSETS, 0x8);

struct XGI_XUSER_GET_PROPERTY {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;  // If xuid is 0 then user_index is used.
  xe::be<uint32_t>
      property_size_ptr;  // Normally filled with sizeof(XUSER_PROPERTY), with
                          // exception of binary and wstring type.
  xe::be<uint32_t> context_address;
  xe::be<uint32_t> property_address;
};
static_assert_size(XGI_XUSER_GET_PROPERTY, 0x20);

struct XGI_XUSER_SET_CONTEXT {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;
  XUSER_CONTEXT context;
};
static_assert_size(XGI_XUSER_SET_CONTEXT, 0x18);

struct XGI_XUSER_SET_PROPERTY {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> property_id;
  xe::be<uint32_t> data_size;
  xe::be<uint32_t> data_address;
};
static_assert_size(XGI_XUSER_SET_PROPERTY, 0x20);

struct XUSER_STATS_VIEW {
  xe::be<uint32_t> ViewId;
  xe::be<uint32_t> TotalViewRows;
  xe::be<uint32_t> NumRows;
  xe::be<uint32_t> pRows;
};

struct XUSER_STATS_COLUMN {
  xe::be<uint16_t> ColumnId;
  X_USER_DATA Value;
};

struct XUSER_STATS_RESET {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> view_id;
};

struct XUSER_ANID {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> cchAnIdBuffer;
  xe::be<uint32_t> pszAnIdBuffer;
  xe::be<uint32_t> value_const;  // 1
};

// The 17559 dashboard reads the game in the drive from the SPA in its
// executable.
struct XGI_TITLE_INFO_FROM_SPA {
  xe::be<uint32_t> spa_address;
  xe::be<uint32_t> spa_size;
  xe::be<uint32_t> name_length;  // In characters.
  xe::be<uint32_t> name_address;
  // The icon buffer's size going in and the icon's coming out.
  xe::be<uint32_t> icon_size_address;
  xe::be<uint32_t> icon_address;
};
static_assert_size(XGI_TITLE_INFO_FROM_SPA, 0x18);

// XamUserReadUserPreference and XamUserWriteUserPreference send this, with
// the value's address for a read and the value for a write.
struct XGI_USER_PREFERENCE {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> preference;
  xe::be<uint32_t> value;
};
static_assert_size(XGI_USER_PREFERENCE, 0xC);

// The console keeps user preferences as bits of one of the dashboard's
// profile settings, read and written by mask. Masks 6 and 0x18 have a bit of
// their own that a write sets, and a read of one never written returns 1.
constexpr uint32_t kUserPreferencesSettingId = 0x10040053;

static bool IsUserPreference(uint32_t preference) {
  switch (preference) {
    case 0x1:
    case 0x6:
    case 0x18:
    case 0x20:
    case 0x40:
    case 0x80:
    case 0x100:
    case 0x200:
      return true;
  }
  return false;
}

static uint32_t UserPreferenceWrittenBit(uint32_t preference) {
  switch (preference) {
    case 0x6:
      return 0x4;
    case 0x18:
      return 0x10;
  }
  return 0;
}

static X_HRESULT ReadUserPreferences(KernelState* kernel_state,
                                     uint32_t user_index, uint32_t& bits) {
  if (user_index >= XUserMaxUserCount) {
    return X_E_INVALIDARG;
  }
  const auto user = kernel_state->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_E_NO_SUCH_USER;
  }
  bits = 0;
  if (const auto setting =
          kernel_state->xam_state()->user_tracker()->GetSetting(
              user, kDashboardID, kUserPreferencesSettingId)) {
    bits = static_cast<uint32_t>(std::get<int32_t>(setting->get_host_data()));
  }
  return X_E_SUCCESS;
}

X_HRESULT XgiApp::ReadUserPreference(uint32_t user_index, uint32_t preference,
                                     uint32_t& value) {
  if (!IsUserPreference(preference)) {
    return X_E_INVALIDARG;
  }
  uint32_t bits = 0;
  if (const X_HRESULT result =
          ReadUserPreferences(kernel_state_, user_index, bits);
      XFAILED(result)) {
    return result;
  }
  value = bits & preference;
  const uint32_t written_bit = UserPreferenceWrittenBit(preference);
  if (!written_bit) {
    return X_E_SUCCESS;
  }
  if (!(value & written_bit)) {
    return 1;
  }
  value &= ~written_bit;
  return X_E_SUCCESS;
}

X_HRESULT XgiApp::WriteUserPreference(uint32_t user_index, uint32_t preference,
                                      uint32_t value) {
  const uint32_t written_bit = UserPreferenceWrittenBit(preference);
  if (user_index >= XUserMaxUserCount || !IsUserPreference(preference) ||
      (value | preference) != preference || (value & written_bit)) {
    return X_E_INVALIDARG;
  }
  uint32_t bits = 0;
  if (const X_HRESULT result =
          ReadUserPreferences(kernel_state_, user_index, bits);
      XFAILED(result)) {
    return result;
  }
  bits = (bits & ~preference) | value | written_bit;
  const auto user = kernel_state_->xam_state()->GetUserProfile(user_index);
  const UserSetting setting(
      static_cast<UserSettingId>(kUserPreferencesSettingId),
      static_cast<int32_t>(bits));
  kernel_state_->xam_state()->user_tracker()->UpsertSetting(
      user->xuid(), kDashboardID, &setting);
  kernel_state_->BroadcastNotification(
      kXNotificationSystemProfileSettingChanged, 1u << user_index);
  return X_E_SUCCESS;
}

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_SET_CONTEXT));
      const XGI_XUSER_SET_CONTEXT* xgi_context =
          reinterpret_cast<const XGI_XUSER_SET_CONTEXT*>(buffer);

      XELOGD("XGIUserSetContext({:08X}, ID: {:08X}, Value: {:08X})",
             xgi_context->user_index.get(),
             xgi_context->context.context_id.get(),
             xgi_context->context.value.get());

      UserProfile* user = nullptr;
      if (xgi_context->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_context->xuid);
      } else {
        user =
            kernel_state_->xam_state()->GetUserProfile(xgi_context->user_index);
      }

      if (user) {
        kernel_state_->xam_state()->user_tracker()->UpdateContext(
            user->xuid(), xgi_context->context.context_id,
            xgi_context->context.value);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_SET_PROPERTY));
      const XGI_XUSER_SET_PROPERTY* xgi_property =
          reinterpret_cast<const XGI_XUSER_SET_PROPERTY*>(buffer);

      XELOGD("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})",
             xgi_property->user_index.get(), xgi_property->property_id.get(),
             xgi_property->data_size.get(), xgi_property->data_address.get());

      UserProfile* user = nullptr;
      if (xgi_property->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_property->xuid);
      } else {
        user = kernel_state_->xam_state()->GetUserProfile(
            xgi_property->user_index);
      }

      if (user) {
        Property property(
            xgi_property->property_id,
            Property::get_valid_data_size(xgi_property->property_id,
                                          xgi_property->data_size),
            memory_->TranslateVirtual<uint8_t*>(xgi_property->data_address));

        kernel_state_->xam_state()->user_tracker()->AddProperty(user->xuid(),
                                                                &property);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(X_USER_ACHIEVEMENT));

      const XGI_WRITEACHIEVEMENT* write_achievements =
          reinterpret_cast<const XGI_WRITEACHIEVEMENT*>(buffer);

      const X_USER_ACHIEVEMENT* achievements =
          memory_->TranslateVirtual<X_USER_ACHIEVEMENT*>(
              write_achievements->achievements_ptr);

      XELOGD("XGIUserWriteAchievements({:08X}, {:08X})",
             write_achievements->num_achievements.get(),
             write_achievements->achievements_ptr.get());

      for (uint32_t i = 0; i < write_achievements->num_achievements; i++) {
        const X_USER_ACHIEVEMENT& achievement = achievements[i];

        kernel_state_->achievement_manager()->EarnAchievement(
            achievement.user_index, kernel_state_->title_id(),
            achievement.achievement_id);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      XELOGD("XSessionCreate({:08X}, {:08X})", buffer_ptr, buffer_length);
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t flags = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t num_slots_public = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t num_slots_private = xe::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t user_index = xe::load_and_swap<uint32_t>(buffer + 0x10);
      uint32_t session_info_ptr = xe::load_and_swap<uint32_t>(buffer + 0x14);
      uint32_t nonce_ptr = xe::load_and_swap<uint32_t>(buffer + 0x18);

      XELOGD(
          "XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
          "{:08X})",
          session_ptr, flags, num_slots_public, num_slots_private, user_index,
          session_info_ptr, nonce_ptr);

      // 584107FB expects offline session creation using flags 0 to succeed
      // while offline.
      // 58410889 expects stats session creation failure while offline.
      //
      // Presence, stats and matchmaking need the creating user signed in to
      // Live.
      constexpr uint32_t kLiveFlags = SessionFlags::PRESENCE |
                                      SessionFlags::STATS |
                                      SessionFlags::MATCHMAKING;
      const UserProfile* user =
          kernel_state_->xam_state()->GetUserProfile(user_index);
      const bool live =
          user && user->signin_state() ==
                      static_cast<uint32_t>(SignInState::SignedInToLive);
      if (flags & kLiveFlags && !live) {
        return 0x80155209;  // X_ONLINE_E_SESSION_NOT_LOGGED_ON
      }
      // Arbitrated peer networking needs the multiplayer privilege, which only
      // Live users hold.
      if (flags & SessionFlags::PEER_NETWORK &&
          flags & SessionFlags::ARBITRATION && !live) {
        return 0x80155201;  // X_ONLINE_E_SESSION_INSUFFICIENT_PRIVILEGES
      }

      auto session = XSession::FromGuest(kernel_state_, session_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->Create(flags, user_index, num_slots_public,
                             num_slots_private, session_info_ptr, nonce_ptr);
    }
    case 0x000B0011: {
      XELOGD("XGISessionDelete({:08X}, {:08X})", buffer_ptr, buffer_length);
      auto session = XSession::FromGuest(kernel_state_,
                                         xe::load_and_swap<uint32_t>(buffer));
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->DeleteSession();
    }
    case 0x000B0012: {
      assert_true(buffer_length == 0x14);
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t user_count = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t xuid_array = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t user_index_array = xe::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t private_slots_array = xe::load_and_swap<uint32_t>(buffer + 0x10);

      // No user indices means remote users join by XUID.
      XELOGD("XGISessionJoin({:08X}, {}, {:08X}, {:08X}, {:08X})", session_ptr,
             user_count, xuid_array, user_index_array, private_slots_array);
      if (auto session = XSession::FromGuest(kernel_state_, session_ptr)) {
        session->Join(user_count, xuid_array, user_index_array,
                      private_slots_array);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0013: {
      assert_true(buffer_length == 0x14);
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t count = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t xuid_array = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t user_index_array = xe::load_and_swap<uint32_t>(buffer + 0xC);

      // No user indices means remote users leave by XUID.
      XELOGD("XGISessionLeave({:08X}, {}, {:08X}, {:08X})", session_ptr, count,
             xuid_array, user_index_array);
      if (auto session = XSession::FromGuest(kernel_state_, session_ptr)) {
        session->Leave(count, xuid_array, user_index_array);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      XELOGD("XSessionStart({:08X})", buffer_ptr);
      if (auto session = XSession::FromGuest(
              kernel_state_, xe::load_and_swap<uint32_t>(buffer))) {
        session->Start();
      }
      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      XELOGD("XSessionEnd({:08X}, {:08X})", buffer_ptr, buffer_length);
      if (auto session = XSession::FromGuest(
              kernel_state_, xe::load_and_swap<uint32_t>(buffer))) {
        session->End();
      }
      return X_STATUS_SUCCESS;
    }
    case 0x000B0018: {
      assert_true(!buffer_length || buffer_length == 0x10);
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t flags = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t max_public_slots = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t max_private_slots = xe::load_and_swap<uint32_t>(buffer + 0xC);
      XELOGD("XSessionModify({:08X}, {:08X}, {}, {})", session_ptr, flags,
             max_public_slots, max_private_slots);
      auto session = XSession::FromGuest(kernel_state_, session_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->Modify(flags, max_public_slots, max_private_slots);
    }
    case 0x000B001A: {
      assert_true(!buffer_length || buffer_length == 0x20);
      const uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      // After the flags: the 64-bit nonce at 0x8 and the duration at 0x10.
      const uint32_t results_size = xe::load_and_swap<uint32_t>(buffer + 0x14);
      const uint32_t results_ptr = xe::load_and_swap<uint32_t>(buffer + 0x18);
      XELOGD("XSessionArbitrationRegister({:08X}, {}, {:08X})", session_ptr,
             results_size, results_ptr);
      auto session = XSession::FromGuest(kernel_state_, session_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->RegisterArbitration(results_size, results_ptr);
    }
    case 0x000B0016:  // XSessionSearch: XSessionSearchEx without num_users.
    case 0x000B001C: {
      assert_true(!buffer_length || buffer_length == 0x20 ||
                  buffer_length == 0x24);
      return SessionSearch(buffer);
    }
    case 0x000B0065: {
      assert_true(!buffer_length || buffer_length == 0x34);
      return SessionSearchWeighted(buffer);
    }
    case 0x000B001B: {  // XSessionSearchByID
      assert_true(!buffer_length || buffer_length == 0x14);
      return SessionSearchByIds(buffer, buffer + 0x4, 1, 0xC, 0x10);
    }
    case 0x000B0060: {  // XSessionSearchByIds
      assert_true(!buffer_length || buffer_length == 0x20);
      const uint32_t count = xe::load_and_swap<uint32_t>(buffer + 0x4);
      const uint32_t ids_ptr = xe::load_and_swap<uint32_t>(buffer + 0x8);
      if (!ids_ptr || !count || count > 100) {
        return X_E_INVALIDARG;
      }
      return SessionSearchByIds(
          buffer, memory_->TranslateVirtual<const uint8_t*>(ids_ptr), count,
          0xC, 0x10);
    }
    case 0x000B001D: {
      assert_true(!buffer_length || buffer_length == 0x18);
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t size_ptr = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t details_ptr = xe::load_and_swap<uint32_t>(buffer + 0x8);
      XELOGD("XSessionGetDetails({:08X}, {:08X}, {:08X})", session_ptr,
             size_ptr, details_ptr);
      auto session = XSession::FromGuest(kernel_state_, session_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->GetDetails(size_ptr, details_ptr);
    }
    case 0x000B0021: {
      struct XUserReadStats {
        xe::be<uint32_t> titleId;
        xe::be<uint32_t> xuids_count;
        xe::be<uint32_t> xuids_guest_address;
        xe::be<uint32_t> specs_count;
        xe::be<uint32_t> specs_guest_address;
        xe::be<uint32_t> results_size;
        xe::be<uint32_t> results_guest_address;
      }* data = reinterpret_cast<XUserReadStats*>(buffer);
      XELOGD("XUserReadStats({:08X}, {} users, {} views, {:08X}, {:08X})",
             uint32_t(data->titleId), uint32_t(data->xuids_count),
             uint32_t(data->specs_count), uint32_t(data->results_size),
             uint32_t(data->results_guest_address));

      // Stats are for users signed in to Live.
      bool live = false;
      for (uint32_t i = 0; i < XUserMaxUserCount && !live; ++i) {
        const UserProfile* user = kernel_state_->xam_state()->GetUserProfile(i);
        live = user && user->signin_state() ==
                           static_cast<uint32_t>(SignInState::SignedInToLive);
      }
      if (!live) {
        return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
      }
      if (!data->specs_count || !data->specs_guest_address) {
        return X_E_INVALIDARG;
      }

      // No stats service keeps rows, so every requested view reads back with
      // none: everyone is unrated.
      using Enumerator = XUserStatsEnumerator;
      const auto* specs = memory_->TranslateVirtual<Enumerator::XUSER_STATS_SPEC*>(
          data->specs_guest_address);
      std::vector<uint32_t> view_ids;
      for (uint32_t i = 0; i < data->specs_count; ++i) {
        view_ids.push_back(specs[i].ViewId);
      }
      // The title passes the size of its results buffer by value.
      const uint32_t needed = Enumerator::EmptyResultsSize(view_ids.size());
      if (!data->results_guest_address || data->results_size < needed) {
        return X_E_INSUFFICIENT_BUFFER;
      }
      Enumerator::WriteEmptyResults(
          memory_->TranslateVirtual(data->results_guest_address),
          data->results_size, data->results_guest_address, view_ids);
      return X_E_SUCCESS;
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      XELOGD("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003C: {
      if (buffer_length != sizeof(XGI_TITLE_INFO_FROM_SPA)) {
        return X_E_INVALIDARG;
      }
      const auto* args = reinterpret_cast<XGI_TITLE_INFO_FROM_SPA*>(buffer);
      if (!args->spa_address || !args->name_address ||
          !args->icon_size_address || !args->icon_address) {
        return X_E_INVALIDARG;
      }
      SpaInfo spa(std::span<uint8_t>(
          memory_->TranslateVirtual<uint8_t*>(args->spa_address),
          args->spa_size));
      spa.Load();
      const XLanguage language = spa.GetExistingLanguage(static_cast<XLanguage>(
          kernel_state_->xconfig()->ReadSetting<uint32_t>(
              XCONFIG_USER_CATEGORY, XCONFIG_USER_LANGUAGE)));
      string_util::copy_and_swap_truncating(
          memory_->TranslateVirtual<char16_t*>(args->name_address),
          xe::to_utf16(spa.title_name(language)), args->name_length);
      const std::span<const uint8_t> icon = spa.title_icon();
      auto* icon_size =
          memory_->TranslateVirtual<xe::be<uint32_t>*>(args->icon_size_address);
      const bool fits = icon.size() <= *icon_size;
      *icon_size = static_cast<uint32_t>(icon.size());
      if (!fits) {
        return X_E_INSUFFICIENT_BUFFER;
      }
      std::memcpy(memory_->TranslateVirtual(args->icon_address), icon.data(),
                  icon.size());
      return X_E_SUCCESS;
    }
    case 0x000B003D: {
      // Used in 5451082A, 5553081E
      // XUserGetCachedANID
      XELOGI("XUserGetANID({:08X}, {:08X}), implemented in netplay", buffer_ptr,
             buffer_length);
      return X_E_FAIL;
    }
    case 0x000B0041: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_GET_PROPERTY));
      const XGI_XUSER_GET_PROPERTY* xgi_property =
          reinterpret_cast<const XGI_XUSER_GET_PROPERTY*>(buffer);

      UserProfile* user = nullptr;
      if (xgi_property->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_property->xuid);
      } else {
        user = kernel_state_->xam_state()->GetUserProfile(
            xgi_property->user_index);
      }

      if (!user) {
        XELOGD(
            "XGIUserGetProperty - Invalid user provided: Index: {:08X} XUID: "
            "{:16X}",
            xgi_property->user_index.get(), xgi_property->xuid.get());
        return X_E_NOTFOUND;
      }

      // Process context
      if (xgi_property->context_address) {
        XUSER_CONTEXT* context = memory_->TranslateVirtual<XUSER_CONTEXT*>(
            xgi_property->context_address);

        XELOGD("XGIUserGetProperty - Context requested: {:08X} XUID: {:16X}",
               context->context_id.get(), user->xuid());

        auto context_value =
            kernel_state_->xam_state()->user_tracker()->GetUserContext(
                user->xuid(), context->context_id);

        if (!context_value) {
          return X_E_INVALIDARG;
        }

        context->value = context_value.value();
        return X_E_SUCCESS;
      }

      if (!xgi_property->property_size_ptr || !xgi_property->property_address) {
        return X_E_INVALIDARG;
      }

      // Process property
      XUSER_PROPERTY* property = memory_->TranslateVirtual<XUSER_PROPERTY*>(
          xgi_property->property_address);

      XELOGD("XGIUserGetProperty - Property requested: {:08X} XUID: {:16X}",
             property->property_id.get(), user->xuid());

      return kernel_state_->xam_state()->user_tracker()->GetProperty(
          user->xuid(),
          memory_->TranslateVirtual<uint32_t*>(xgi_property->property_size_ptr),
          property);
    }
    case 0x000B0071: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_AWARD_AVATAR_ASSETS));
      const XGI_AWARD_AVATAR_ASSETS* award_avatar_assets =
          reinterpret_cast<const XGI_AWARD_AVATAR_ASSETS*>(buffer);

      XELOGD("XUserAwardAvatarAssets({:08X}, {:08X})",
             award_avatar_assets->num_assets.get(),
             award_avatar_assets->assets_ptr.get());

      const X_USER_AVATAR_ASSET* avatar_assets =
          memory_->TranslateVirtual<X_USER_AVATAR_ASSET*>(
              award_avatar_assets->assets_ptr);

      for (uint32_t i = 0; i < award_avatar_assets->num_assets; i++) {
        const X_USER_AVATAR_ASSET& avatar_asset = avatar_assets[i];

        const auto user =
            kernel_state_->xam_state()->GetUserProfile(avatar_asset.user_index);

        if (user) {
          XELOGI("Player: {} Unlocked Avatar Award Asset ID: {}", user->name(),
                 avatar_asset.award_id.get());
        }
      }

      return X_E_SUCCESS;
    }
    case 0x000B0082: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_USER_PREFERENCE));
      const auto* request =
          reinterpret_cast<const XGI_USER_PREFERENCE*>(buffer);
      if (!request->value) {
        return X_E_INVALIDARG;
      }
      uint32_t value = 0;
      const X_HRESULT result =
          ReadUserPreference(request->user_index, request->preference, value);
      if (XSUCCEEDED(result)) {
        xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(request->value),
                                     value);
      }
      return result;
    }
    case 0x000B0083: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_USER_PREFERENCE));
      const auto* request =
          reinterpret_cast<const XGI_USER_PREFERENCE*>(buffer);
      return WriteUserPreference(request->user_index, request->preference,
                                 request->value);
    }
  }
  XELOGE(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

// Writes an XSESSION_SEARCHRESULT_HEADER and as many of |sessions| as fit in
// |buffer_size| (at most |max_results|): the results, then each one's
// properties and contexts. Returns how many it wrote.
uint32_t XgiApp::WriteSearchResults(
    const std::vector<LiveSessionAdvert>& sessions, uint32_t max_results,
    uint32_t buffer_size, uint32_t results_ptr) {
  // The header, the results, then each result's properties and contexts.
  constexpr uint32_t kResultSize = 0x5C;
  uint32_t count = std::min<uint32_t>(uint32_t(sessions.size()), max_results);
  auto needed = [&](uint32_t n) {
    uint32_t size = 8 + n * kResultSize;
    for (uint32_t i = 0; i < n; ++i) {
      size += uint32_t(sessions[i].properties.size()) * 0x18 +
              uint32_t(sessions[i].contexts.size()) * 8;
    }
    return size;
  };
  while (count && needed(count) > buffer_size) {
    --count;
  }

  auto* header = memory_->TranslateVirtual<uint8_t*>(results_ptr);
  std::memset(header, 0, std::min(buffer_size, needed(count)));
  xe::store_and_swap<uint32_t>(header, count);
  xe::store_and_swap<uint32_t>(header + 4, count ? results_ptr + 8 : 0);
  uint32_t extra = results_ptr + 8 + count * kResultSize;
  for (uint32_t i = 0; i < count; ++i) {
    const auto& session = sessions[i];
    auto* result =
        memory_->TranslateVirtual<uint8_t*>(results_ptr + 8 + i * kResultSize);
    std::memcpy(result, &session.info, sizeof(session.info));
    xe::store_and_swap<uint32_t>(
        result + 0x3C, session.max_public_slots - session.used_public_slots);
    xe::store_and_swap<uint32_t>(
        result + 0x40,
        session.max_private_slots -
            std::min(session.used_private_slots, session.max_private_slots));
    xe::store_and_swap<uint32_t>(result + 0x44, session.used_public_slots);
    xe::store_and_swap<uint32_t>(result + 0x48, session.used_private_slots);
    xe::store_and_swap<uint32_t>(result + 0x4C,
                                 uint32_t(session.properties.size()));
    xe::store_and_swap<uint32_t>(result + 0x50,
                                 uint32_t(session.contexts.size()));
    xe::store_and_swap<uint32_t>(result + 0x54,
                                 session.properties.empty() ? 0 : extra);
    for (const auto& property : session.properties) {
      auto* out = memory_->TranslateVirtual<uint8_t*>(extra);
      xe::store_and_swap<uint32_t>(out, property.id);
      std::memcpy(out + 8, property.data.data(), property.data.size());
      extra += 0x18;
    }
    xe::store_and_swap<uint32_t>(result + 0x58,
                                 session.contexts.empty() ? 0 : extra);
    for (const auto& context : session.contexts) {
      auto* out = memory_->TranslateVirtual<uint8_t*>(extra);
      xe::store_and_swap<uint32_t>(out, context.id);
      xe::store_and_swap<uint32_t>(out + 4, context.value);
      extra += 8;
    }
  }
  return count;
}

// XSessionSearchByID(s): the announced sessions with the given IDs, full or
// not (invites and joining a friend look a session up by its ID). A size of 0
// asks for the size needed.
X_HRESULT XgiApp::SessionSearchByIds(uint8_t* args, const uint8_t* ids,
                                     uint32_t id_count,
                                     uint32_t size_offset,
                                     uint32_t results_offset) {
  const uint32_t user_index = xe::load_and_swap<uint32_t>(args);
  const uint32_t buffer_size = xe::load_and_swap<uint32_t>(args + size_offset);
  const uint32_t results_ptr =
      xe::load_and_swap<uint32_t>(args + results_offset);
  constexpr uint32_t kResultSize = 0x5C;
  if (user_index >= XUserMaxUserCount) {
    return X_E_INVALIDARG;
  }
  if (!buffer_size) {
    xe::store_and_swap<uint32_t>(args + size_offset,
                                 8 + id_count * kResultSize);
    return 0x80155207;  // X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER
  }
  if (!results_ptr || buffer_size < 8) {
    return X_E_INVALIDARG;
  }
  auto sessions = FindLiveSessions(kernel_state_, 1500);
  std::erase_if(sessions, [&](const LiveSessionAdvert& session) {
    for (uint32_t i = 0; i < id_count; ++i) {
      if (!std::memcmp(session.info.session_id.data(), ids + i * 8, 8)) {
        return false;
      }
    }
    return true;
  });
  const uint32_t count =
      WriteSearchResults(sessions, id_count, buffer_size, results_ptr);
  XELOGI("XSessionSearchByIds({} IDs) -> {} found", id_count, count);
  return X_E_SUCCESS;
}

X_HRESULT XgiApp::SessionSearch(const uint8_t* args) {
  SearchQuery query;
  query.proc_index = xe::load_and_swap<uint32_t>(args + 0x0);
  query.max_results = xe::load_and_swap<uint32_t>(args + 0x8);
  query.num_props = xe::load_and_swap<uint16_t>(args + 0xC);
  query.num_contexts = xe::load_and_swap<uint16_t>(args + 0xE);
  query.props_ptr = xe::load_and_swap<uint32_t>(args + 0x10);
  query.contexts_ptr = xe::load_and_swap<uint32_t>(args + 0x14);
  query.buffer_size = xe::load_and_swap<uint32_t>(args + 0x18);
  query.results_ptr = xe::load_and_swap<uint32_t>(args + 0x1C);
  return SessionSearch(query, "XSessionSearch");
}

// XSessionSearchWeighted (Halo: Reach): XSessionSearchEx plus properties and
// contexts the server ranks results by. Those only order results on Live,
// so they're logged and the rest is an ordinary search.
X_HRESULT XgiApp::SessionSearchWeighted(const uint8_t* args) {
  SearchQuery query;
  query.proc_index = xe::load_and_swap<uint32_t>(args + 0x0);
  query.max_results = xe::load_and_swap<uint32_t>(args + 0x8);
  const uint16_t num_weighted_props = xe::load_and_swap<uint16_t>(args + 0xC);
  const uint16_t num_weighted_contexts =
      xe::load_and_swap<uint16_t>(args + 0xE);
  const uint32_t weighted_props_ptr = xe::load_and_swap<uint32_t>(args + 0x10);
  const uint32_t weighted_contexts_ptr =
      xe::load_and_swap<uint32_t>(args + 0x14);
  query.num_props = xe::load_and_swap<uint16_t>(args + 0x18);
  query.num_contexts = xe::load_and_swap<uint16_t>(args + 0x1A);
  query.props_ptr = xe::load_and_swap<uint32_t>(args + 0x1C);
  query.contexts_ptr = xe::load_and_swap<uint32_t>(args + 0x20);
  query.buffer_size = xe::load_and_swap<uint32_t>(args + 0x24);
  query.results_ptr = xe::load_and_swap<uint32_t>(args + 0x28);
  std::string weighted;
  for (uint32_t i = 0; weighted_contexts_ptr && i < num_weighted_contexts;
       ++i) {
    const auto* context = memory_->TranslateVirtual<const uint8_t*>(
        weighted_contexts_ptr + i * 8);
    weighted +=
        fmt::format(" ctx {:08X}={}", xe::load_and_swap<uint32_t>(context),
                    xe::load_and_swap<uint32_t>(context + 4));
  }
  for (uint32_t i = 0; weighted_props_ptr && i < num_weighted_props; ++i) {
    const auto* property = memory_->TranslateVirtual<const uint8_t*>(
        weighted_props_ptr + i * 0x18);
    weighted += fmt::format(" prop {:08X}={:016X}",
                            xe::load_and_swap<uint32_t>(property),
                            xe::load_and_swap<uint64_t>(property + 0x8));
  }
  XELOGI("XSessionSearchWeighted: weighted{}", weighted);
  return SessionSearch(query, "XSessionSearchWeighted");
}

X_HRESULT XgiApp::SessionSearch(const SearchQuery& search,
                                std::string_view name) {
  const uint32_t proc_index = search.proc_index;
  const uint32_t max_results = search.max_results;
  const uint16_t num_props = search.num_props;
  const uint16_t num_contexts = search.num_contexts;
  const uint32_t props_ptr = search.props_ptr;
  const uint32_t contexts_ptr = search.contexts_ptr;
  const uint32_t buffer_size = search.buffer_size;
  const uint32_t results_ptr = search.results_ptr;
  if (!results_ptr || buffer_size < 8) {
    return X_E_INVALIDARG;
  }

  // Live matched the query procedure on its servers. Here a session is left
  // out only when it was made for another game context than the searcher
  // asks for.
  // TODO(xenios-jp): run the title's query (proc_index in its SPA: filters and
  // comparisons on properties, sometimes skill ranges). Titles that rely on
  // it can match too loosely now. Halo 3, Gears of War and Virtua Fighter 5
  // matched correctly with context matching alone.
  std::vector<std::pair<uint32_t, uint32_t>> wanted;
  std::string query;
  for (uint32_t i = 0; contexts_ptr && i < num_contexts; ++i) {
    const auto* context =
        memory_->TranslateVirtual<const uint8_t*>(contexts_ptr + i * 8);
    wanted.emplace_back(xe::load_and_swap<uint32_t>(context),
                        xe::load_and_swap<uint32_t>(context + 4));
    query += fmt::format(" ctx {:08X}={}", wanted.back().first,
                         wanted.back().second);
  }
  for (uint32_t i = 0; props_ptr && i < num_props; ++i) {
    const auto* property =
        memory_->TranslateVirtual<const uint8_t*>(props_ptr + i * 0x18);
    query += fmt::format(" prop {:08X}={:016X}",
                         xe::load_and_swap<uint32_t>(property),
                         xe::load_and_swap<uint64_t>(property + 0x8));
  }

  auto sessions = FindLiveSessions(kernel_state_, 1500);
  std::erase_if(sessions, [&](const LiveSessionAdvert& session) {
    if (session.used_public_slots >= session.max_public_slots) {
      return true;
    }
    for (const auto& [id, value] : wanted) {
      for (const auto& context : session.contexts) {
        if (context.id == id && context.value != value) {
          return true;
        }
      }
    }
    return false;
  });

  const uint32_t count =
      WriteSearchResults(sessions, max_results, buffer_size, results_ptr);
  XELOGI("{}(proc {}):{} -> {} of {} announced", name, proc_index, query,
         count, sessions.size());
  return X_E_SUCCESS;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
