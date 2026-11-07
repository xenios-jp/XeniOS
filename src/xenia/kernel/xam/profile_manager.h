/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_PROFILE_MANAGER_H_
#define XENIA_KERNEL_XAM_PROFILE_MANAGER_H_

#include <bitset>
#include <random>
#include <string>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/string.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
class KernelState;
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {
class UserTracker;
}  // namespace xam
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

struct ScannedTitleInfo {
  uint32_t title_id;
  std::string title_name;
  std::filesystem::path path_to_file;
  std::vector<GpdInfoProfile::DiscInfo> all_discs;
  time_t last_run_time;
};

inline const std::string kDashboardStringID =
    fmt::format("{:08X}", kDashboardID);

constexpr std::string_view kDefaultMountFormat = "User_{:016X}";

const static inline uint64_t GenerateXuid() {
  std::random_device rd;
  std::mt19937 gen(rd());

  return ((uint64_t)0xE03 << 52) + (gen() % (1 << 31));
}

// Online XUIDs keep Live's 0x0009 prefix, which titles check, and use the high
// 0x0009F part of it with 44 random bits, away from the low part that real Live
// accounts (and profiles copied from a real console) use. The online service
// may still replace one that another player already holds.
const static inline uint64_t GenerateOnlineXuid() {
  std::random_device rd;
  std::mt19937_64 gen((uint64_t(rd()) << 32) | rd());

  return 0x0009F00000000000ULL | (gen() & 0x00000FFFFFFFFFFFULL);
}

class ProfileManager {
 public:
  static bool DecryptAccountFile(const uint8_t* data, X_XAMACCOUNTINFO* output,
                                 bool devkit = false);

  static void EncryptAccountFile(const X_XAMACCOUNTINFO* input, uint8_t* output,
                                 bool devkit = false);

  // Profile:
  //  - Account
  //  - GPDs (Dashboard, titles)

  // Loading Profile means load everything
  // Loading Account means load basic data
  ProfileManager(KernelState* kernel_state, UserTracker* user_tracker);

  ~ProfileManager() = default;

  bool CreateProfile(const std::string gamertag, bool autologin,
                     bool default_xuid = false);
  bool CreateProfile(const X_XAMACCOUNTINFO* account_info, uint64_t xuid);

  bool DeleteProfile(const uint64_t xuid);

  bool MountProfile(const uint64_t xuid, std::string mount_path = "");
  bool DismountProfile(const uint64_t xuid);

  void Login(const uint64_t xuid, const uint8_t user_index = XUserIndexAny,
             bool notify = true);
  void Logout(const uint8_t user_index, bool notify = true);
  void LoginMultiple(const std::map<uint8_t, uint64_t>& profiles);

  bool LoadAccount(const uint64_t xuid);

  void ReloadProfiles();
  void ReloadProfile(const uint64_t xuid);
  void ReloadProfileGpds();
  void SyncProfilesWithConfig();

  UserProfile* GetProfile(const uint64_t xuid) const;
  UserProfile* GetProfile(const uint8_t user_index) const;
  UserProfile* GetProfileLive(const uint64_t xuid) const;
  uint8_t GetUserIndexAssignedToProfile(const uint64_t xuid) const;
  uint8_t GetUserIndexAssignedToLiveProfile(const uint64_t xuid_online) const;

  const std::map<uint64_t, X_XAMACCOUNTINFO>* GetAccounts() {
    return &accounts_;
  }
  const X_XAMACCOUNTINFO* GetAccount(const uint64_t xuid);

  // The profile's picture: its personal picture if it has one, else its gamer
  // picture, each at 64 px before 32 px. Empty if it has none.
  std::vector<uint8_t> GetAccountIcon(const uint64_t xuid);

  uint32_t GetAccountCount() const {
    return static_cast<uint32_t>(accounts_.size());
  }
  bool IsAnyProfileSignedIn() const { return !logged_profiles_.empty(); }

  std::filesystem::path GetProfileContentPath(
      const uint64_t xuid, const uint32_t title_id = -1,
      const XContentType content_type = XContentType::kInvalid) const;

  // Writes the account and applies it to the signed-in profile, filling in
  // what a Live-enabled account is missing.
  bool UpdateAccount(const uint64_t xuid, const X_XAMACCOUNTINFO* account);

  std::vector<ScannedTitleInfo> ScanAllProfilesForTitles() const;

  static bool IsGamertagValid(const std::string gamertag);

 private:
  // For testing without the UI: makes sure a Live-enabled profile with this
  // gamertag exists and returns its XUID (0 on failure).
  uint64_t EnsureLiveProfile(const std::string& gamertag);

  void UpdateConfig(const uint64_t xuid, const uint8_t slot);
  // Tells titles and the host UI which slots are signed in now.
  void NotifySignInChanged();
  bool CreateAccount(const uint64_t xuid, const std::string gamertag);
  bool CreateAccount(const uint64_t xuid, const X_XAMACCOUNTINFO* account);
  bool WriteAccount(const uint64_t xuid, const X_XAMACCOUNTINFO* account);
  bool SetLiveDefaults(X_XAMACCOUNTINFO* account) const;

  std::filesystem::path GetProfilePath(const uint64_t xuid) const;
  std::filesystem::path GetProfilePath(const std::string xuid) const;

  std::vector<uint64_t> FindProfiles() const;

  uint8_t FindFirstFreeProfileSlot() const;
  std::bitset<XUserMaxUserCount> GetUsedUserSlots() const;

  std::map<uint64_t, X_XAMACCOUNTINFO> accounts_;
  std::map<uint8_t, std::unique_ptr<UserProfile>> logged_profiles_;

  KernelState* kernel_state_;
  UserTracker* user_tracker_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_PROFILE_MANAGER_H_
