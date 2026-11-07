/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_PROFILE_PACKAGE_H_
#define XENIA_KERNEL_XAM_PROFILE_PACKAGE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// A profile moved between devices as one file, <Gamertag>.xeniosprofile.
// Sideloaded apps get no iCloud, so the file goes by AirDrop or Files, and the
// app on the other device (XeniOS on iOS, Xenia on a desktop) imports it.
//
// This works on the content folder only, with no emulator running: the callers
// sign the profile out of a game first (or refuse) and tell ProfileManager to
// look again afterwards.
//
// The file:
//   "XENIOSPF" (8 bytes), u32 LE format version, u32 LE manifest size,
//   the manifest: UTF-8 JSON, not compressed, so Inspect reads only the start,
//   then one zstd frame (with its checksum) holding every file the manifest
//   lists, in the manifest's order, each as:
//     u16 LE path size, the path, u64 LE size, i64 LE mtime (Unix seconds), the
//     data,
//   ended by an entry with a path size of 0.
// Paths are UTF-8, '/'-separated and relative to content_root/<XUID>/. The
// first file is always the profile's Account.
//
// What a package may hold, for profile <XUID> (16 hex digits) and any title
// <TITLE> (8 hex digits):
//   FFFE07D1/00010000/<XUID>/...      the profile: Account, GPDs, gamer
//   pictures <TITLE>/00000001/...              saved games
//   <TITLE>/Headers/00000001/...      their content headers
// Nothing else: never the device key or anything else of the online account,
// which lives in storage_root/live and stays with the device.

namespace xe {
namespace kernel {
namespace xam {

constexpr uint32_t kProfilePackageVersion = 1;
inline constexpr std::string_view kProfilePackageExtension = ".xeniosprofile";

enum class ProfilePackageError {
  kNone = 0,
  kOpenFailed,   // The file can't be opened or created.
  kBadFile,      // Not a profile file, damaged, or holding paths it may not.
  kNewerFormat,  // Made by a newer app: update this one.
  kNoProfile,    // Export: no such profile here.
  // Import: the profile is signed in and playing. Quit the game first.
  kProfileInUse,
  kLowDiskSpace,  // Import: not enough free space for it.
  kTooLarge,      // Over the size or file-count caps.
  kReadFailed,    // Export: a profile file couldn't be read (or changed).
  kWriteFailed,   // A file couldn't be written.
};
const char* ProfilePackageErrorName(ProfilePackageError error);

// Bytes, for the export screen. The account and the gamer pictures count as
// settings, with the dashboard GPD (FFFE07D1.gpd); the games' GPDs are
// achievements.
struct ProfileDataSizes {
  uint64_t saves = 0;
  uint64_t achievements = 0;
  uint64_t settings = 0;
};

struct ProfilePackageFile {
  std::string path;  // Relative to content_root/<XUID>/, '/'-separated.
  uint64_t size = 0;
  int64_t mtime = 0;  // Unix seconds.
};

// A game with saves in the package.
struct ProfilePackageTitle {
  uint32_t title_id = 0;
  // From the dashboard GPD; empty when it doesn't know the game.
  std::string name;
  uint64_t bytes = 0;
  uint32_t saves = 0;  // Saved-game packages (folders).
};

struct ProfilePackageManifest {
  uint32_t format_version = kProfilePackageVersion;
  std::string gamertag;
  uint64_t xuid = 0;         // The profile's own (offline) XUID.
  uint64_t online_xuid = 0;  // Its online account, 0 when it has none.
  bool live_enabled = false;
  // What made it: "XeniOS 1.4 (1234)", "Xenia-edge master@abc1234".
  std::string app;
  std::string platform;  // "iphone", "ipad", "mac", "windows", "linux".
  int64_t created = 0;   // Unix seconds.
  bool saves = false;
  bool achievements = false;
  bool settings = false;
  std::vector<ProfilePackageTitle> titles;
  ProfileDataSizes sizes;
  uint64_t total_bytes = 0;
  std::vector<ProfilePackageFile> files;
};

struct ProfileExportOptions {
  bool saves = true;
  bool achievements = true;
  bool settings = true;
  std::string app;
  std::string platform;
};

// Bytes done of the total; called on the working thread.
using ProfilePackageProgress =
    std::function<void(uint64_t done_bytes, uint64_t total_bytes)>;

// Where profile |xuid| lives: content_root/<XUID>/FFFE07D1/00010000/<XUID>.
std::filesystem::path ProfilePackageProfileFolder(
    const std::filesystem::path& content_root, uint64_t xuid);
// Whether content_root holds profile |xuid| (its Account file).
bool ProfilePackageProfileExists(const std::filesystem::path& content_root,
                                 uint64_t xuid);

ProfileDataSizes MeasureProfileData(const std::filesystem::path& content_root,
                                    uint64_t xuid);

// "<Gamertag>.xeniosprofile", with characters a file name can't hold replaced.
std::string ProfilePackageFileName(std::string_view gamertag);

// Writes profile |xuid| to |out_path| (replacing it). The account always goes;
// the GPDs with achievements or settings; the saved games with saves. A failed
// export leaves no file.
ProfilePackageError ExportProfilePackage(
    const std::filesystem::path& content_root, uint64_t xuid,
    const ProfileExportOptions& options, const std::filesystem::path& out_path,
    const ProfilePackageProgress& progress = {},
    ProfilePackageManifest* out_manifest = nullptr);

// Reads the manifest, for the confirm screen. Checks every path it lists, and
// that the package's Account is the profile the manifest names.
ProfilePackageError InspectProfilePackage(const std::filesystem::path& path,
                                          ProfilePackageManifest* out_manifest);

// The profile here that a package is for: the one with its XUID, else one with
// its online account (a device that joined the account with a link code made
// its own profile, with another XUID). 0 when there's none: the import makes a
// new one.
uint64_t FindProfileForPackage(const std::filesystem::path& content_root,
                               const ProfilePackageManifest& manifest);

enum class ProfileImportMode {
  // Each file of the profile, and each saved game as a whole, comes from
  // whichever side is newer (a tie keeps this device's).
  kKeepNewer,
  // The package's always win.
  kReplace,
};

struct ProfileImportOptions {
  ProfileImportMode mode = ProfileImportMode::kKeepNewer;
  // The profile to import into: 0 for the package's own XUID (made if missing).
  // Any other is a profile here for the same online account
  // (FindProfileForPackage).
  uint64_t target_xuid = 0;
  // Whether the profile is signed in and in a game: asked before the package is
  // read and again before anything of this device's changes.
  std::function<bool(uint64_t xuid)> profile_in_use;
  // Where replaced files go, under profile-import-<date>/<XUID>/. Empty:
  // <content_root>/../backups.
  std::filesystem::path backup_root;
  // Free space to leave after the import.
  uint64_t reserve_bytes = 256ull * 1024 * 1024;
};

struct ProfileImportResult {
  uint64_t xuid = 0;     // The profile imported into.
  bool created = false;  // It wasn't on this device before.
  uint32_t written = 0;  // Files written.
  uint32_t kept =
      0;  // Files or saves where this device's were newer and stayed.
  uint32_t replaced = 0;  // Files or saves replaced, each backed up first.
  std::filesystem::path backup_folder;  // Empty when nothing was replaced.
  ProfilePackageManifest manifest;
};

// Imports a package into content_root. Every path is checked first, and the
// files to write are read into a staging folder next to content_root: a
// damaged package changes nothing. Then each file or saved game replaced goes
// to the backup and the package's is moved in; a move that fails puts back
// everything moved, so the profile is all one or all the other.
ProfilePackageError ImportProfilePackage(
    const std::filesystem::path& content_root,
    const std::filesystem::path& package_path,
    const ProfileImportOptions& options,
    const ProfilePackageProgress& progress = {},
    ProfileImportResult* out_result = nullptr);

// Whether a package for profile |xuid| may hold |path| (see the list above): no
// "..", nothing absolute, nothing outside the profile's folders.
bool IsProfilePackagePathAllowed(std::string_view path, uint64_t xuid);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_PROFILE_PACKAGE_H_
