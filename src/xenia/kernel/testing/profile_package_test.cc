/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/profile_package.h"
#include "xenia/kernel/xam/xam.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using namespace xe::kernel::xam;
namespace fs = std::filesystem;

constexpr uint64_t kXuid = 0xE030000012345678ULL;
constexpr uint64_t kOnlineXuid = 0x0009F00000ABCDEFULL;
const char* kTitle = "4D5307E6";

// A folder of its own under the temp directory, removed at the end.
struct TempFolder {
  TempFolder() {
    std::random_device rd;
    path = fs::temp_directory_path() /
           ("xenia-profile-package-test-" + std::to_string(rd()) + "-" +
            std::to_string(rd()));
    fs::create_directories(path);
  }
  ~TempFolder() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  fs::path path;
};

void WriteText(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  REQUIRE(xe::filesystem::WriteAllBytes(
      path, std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(text.data()), text.size())));
}

std::string ReadText(const fs::path& path) {
  const auto data = xe::filesystem::ReadAllBytes(path);
  return std::string(data.begin(), data.end());
}

// Moves a file's time by |seconds|.
void Age(const fs::path& path, int seconds) {
  fs::last_write_time(
      path, fs::last_write_time(path) + std::chrono::seconds(seconds));
}

int64_t Seconds(const fs::path& path) {
  const auto info = xe::filesystem::GetInfo(path);
  REQUIRE(info);
  return int64_t(info->write_timestamp / 10000000ULL);
}

fs::path ProfileRoot(const fs::path& content) {
  return content / fmt::format("{:016X}", kXuid);
}

// A profile like the emulator leaves it: Account, a GPD, a picture, two saved
// games.
void MakeProfile(const fs::path& content, const std::string& gamertag) {
  X_XAMACCOUNTINFO account = {};
  xe::string_util::copy_and_swap_truncating(
      account.gamertag, xe::to_utf16(gamertag), xe::countof(account.gamertag));
  account.ToggleLiveFlag(true);
  account.xuid_online = kOnlineXuid;
  std::vector<uint8_t> encrypted(sizeof(X_XAMACCOUNTINFO) + 0x18);
  ProfileManager::EncryptAccountFile(&account, encrypted.data());
  const fs::path folder = ProfilePackageProfileFolder(content, kXuid);
  fs::create_directories(folder);
  REQUIRE(xe::filesystem::WriteAllBytes(folder / "Account", encrypted));
  WriteText(folder / (std::string(kTitle) + ".gpd"), "title gpd");
  WriteText(folder / "tile_64.png", "picture");
  const fs::path root = ProfileRoot(content);
  WriteText(root / kTitle / "00000001" / "SaveA" / "data.bin", "save a");
  WriteText(root / kTitle / "Headers" / "00000001" / "SaveA.header",
            "header a");
  WriteText(root / kTitle / "00000001" / "SaveB" / "data.bin", "save b");
}

std::vector<uint8_t> Header(uint32_t version, const std::string& manifest) {
  std::vector<uint8_t> out = {'X', 'E', 'N', 'I', 'O', 'S', 'P', 'F'};
  for (uint32_t value : {version, uint32_t(manifest.size())}) {
    for (int i = 0; i < 4; ++i) {
      out.push_back(uint8_t(value >> (8 * i)));
    }
  }
  out.insert(out.end(), manifest.begin(), manifest.end());
  return out;
}

std::string ManifestWithPath(const std::string& path) {
  return fmt::format(
      R"({{"format":1,"gamertag":"Bo","xuid":"{:016X}","total_bytes":1,)"
      R"("files":[{{"path":"FFFE07D1/00010000/{:016X}/Account","size":0,"mtime":0}},)"
      R"({{"path":"{}","size":1,"mtime":0}}]}})",
      kXuid, kXuid, path);
}

}  // namespace

TEST_CASE("Profile package paths stay inside the profile",
          "[profile_package]") {
  const std::string profile = fmt::format("FFFE07D1/00010000/{:016X}", kXuid);
  CHECK(IsProfilePackagePathAllowed(profile + "/Account", kXuid));
  CHECK(IsProfilePackagePathAllowed(profile + "/4D5307E6.gpd", kXuid));
  CHECK(IsProfilePackagePathAllowed("4D5307E6/00000001/Save/data", kXuid));
  CHECK(IsProfilePackagePathAllowed("4D5307E6/Headers/00000001/Save.header",
                                    kXuid));

  // Another profile's folder.
  CHECK_FALSE(IsProfilePackagePathAllowed(
      "FFFE07D1/00010000/E030000000000001/Account", kXuid));
  // The profile folder itself, with nothing in it.
  CHECK_FALSE(IsProfilePackagePathAllowed(profile, kXuid));
  // Content that isn't saves: DLC, title updates.
  CHECK_FALSE(IsProfilePackagePathAllowed("4D5307E6/00000002/Dlc/x", kXuid));
  CHECK_FALSE(IsProfilePackagePathAllowed("4D5307E6/000B0000/Tu/x", kXuid));
  CHECK_FALSE(IsProfilePackagePathAllowed("4d5307e6/00000001/Save/x", kXuid));
  CHECK_FALSE(IsProfilePackagePathAllowed("live/device_key", kXuid));
  // Climbing out, absolute paths, other separators, drive letters.
  CHECK_FALSE(IsProfilePackagePathAllowed("4D5307E6/00000001/../../x", kXuid));
  CHECK_FALSE(IsProfilePackagePathAllowed("4D5307E6/00000001/./x", kXuid));
  CHECK_FALSE(IsProfilePackagePathAllowed("/4D5307E6/00000001/x", kXuid));
  CHECK_FALSE(IsProfilePackagePathAllowed("4D5307E6/00000001//x", kXuid));
  CHECK_FALSE(
      IsProfilePackagePathAllowed("4D5307E6/00000001/a\\..\\..", kXuid));
  CHECK_FALSE(IsProfilePackagePathAllowed("4D5307E6/00000001/C:x", kXuid));
  CHECK_FALSE(IsProfilePackagePathAllowed(
      std::string("4D5307E6/00000001/a\0b", 21), kXuid));
  CHECK_FALSE(IsProfilePackagePathAllowed("", kXuid));
  CHECK_FALSE(IsProfilePackagePathAllowed(
      "4D5307E6/00000001/" + std::string(300, 'a'), kXuid));
}

TEST_CASE("Profile package file names", "[profile_package]") {
  CHECK(ProfilePackageFileName("Bo Peep") == "Bo Peep.xeniosprofile");
  CHECK(ProfilePackageFileName("a/b:c") == "a_b_c.xeniosprofile");
  CHECK(ProfilePackageFileName("..") == "Profile.xeniosprofile");
  CHECK(ProfilePackageFileName("") == "Profile.xeniosprofile");
}

TEST_CASE("Profile packages round trip", "[profile_package]") {
  TempFolder temp;
  const fs::path from = temp.path / "a" / "content";
  const fs::path to = temp.path / "b" / "content";
  MakeProfile(from, "Bo Peep");
  const fs::path package = temp.path / "Bo Peep.xeniosprofile";

  ProfileExportOptions options;
  options.app = "test";
  options.platform = "mac";
  ProfilePackageManifest exported;
  REQUIRE(ExportProfilePackage(from, kXuid, options, package, {}, &exported) ==
          ProfilePackageError::kNone);
  CHECK_FALSE(fs::exists(fs::path(package).concat(".part")));

  ProfilePackageManifest manifest;
  REQUIRE(InspectProfilePackage(package, &manifest) ==
          ProfilePackageError::kNone);
  CHECK(manifest.gamertag == "Bo Peep");
  CHECK(manifest.xuid == kXuid);
  CHECK(manifest.online_xuid == kOnlineXuid);
  CHECK(manifest.live_enabled);
  CHECK(manifest.platform == "mac");
  REQUIRE(manifest.titles.size() == 1);
  CHECK(manifest.titles[0].title_id == 0x4D5307E6);
  CHECK(manifest.titles[0].saves == 2);
  CHECK(manifest.files.size() == 6);
  CHECK(manifest.files[0].path ==
        fmt::format("FFFE07D1/00010000/{:016X}/Account", kXuid));
  CHECK(manifest.sizes.saves == 6 + 8 + 6);

  const auto sizes = MeasureProfileData(from, kXuid);
  CHECK(sizes.saves == manifest.sizes.saves);
  CHECK(sizes.achievements == 9);

  CHECK(FindProfileForPackage(to, manifest) == 0);
  ProfileImportOptions import_options;
  import_options.backup_root = temp.path / "backups";
  uint64_t last_done = 0, last_total = 0;
  ProfileImportResult result;
  REQUIRE(ImportProfilePackage(
              to, package, import_options,
              [&](uint64_t done, uint64_t total) {
                last_done = done;
                last_total = total;
              },
              &result) == ProfilePackageError::kNone);
  CHECK(result.created);
  CHECK(result.xuid == kXuid);
  CHECK(result.written == 6);
  CHECK(result.replaced == 0);
  CHECK(result.backup_folder.empty());
  CHECK(last_done == last_total);
  CHECK(ProfilePackageProfileExists(to, kXuid));

  for (const auto& file : manifest.files) {
    const fs::path a = ProfileRoot(from) / file.path;
    const fs::path b = ProfileRoot(to) / file.path;
    CHECK(ReadText(a) == ReadText(b));
    CHECK(Seconds(a) == Seconds(b));
  }
  // Nothing left behind from the writes.
  for (const auto& entry : fs::recursive_directory_iterator(to)) {
    CHECK(entry.path().extension() != ".xeniosimport");
  }
}

TEST_CASE("Profile package options leave things out", "[profile_package]") {
  TempFolder temp;
  const fs::path content = temp.path / "content";
  MakeProfile(content, "Bo");
  const fs::path package = temp.path / "Bo.xeniosprofile";
  ProfileExportOptions options;
  options.saves = false;
  options.achievements = false;
  options.settings = false;
  ProfilePackageManifest manifest;
  REQUIRE(ExportProfilePackage(content, kXuid, options, package, {},
                               &manifest) == ProfilePackageError::kNone);
  // The Account and the picture: no GPDs, no saves.
  CHECK(manifest.files.size() == 2);
  CHECK(manifest.titles.empty());
  CHECK(ExportProfilePackage(content, 0xE030000000000001ULL, options,
                             package) == ProfilePackageError::kNoProfile);
}

TEST_CASE("Profile package imports keep the newer save", "[profile_package]") {
  TempFolder temp;
  const fs::path from = temp.path / "a" / "content";
  const fs::path to = temp.path / "b" / "content";
  MakeProfile(from, "Bo");
  const fs::path package = temp.path / "Bo.xeniosprofile";
  REQUIRE(ExportProfilePackage(from, kXuid, {}, package) ==
          ProfilePackageError::kNone);
  ProfileImportOptions options;
  options.backup_root = temp.path / "backups";
  REQUIRE(ImportProfilePackage(to, package, options) ==
          ProfilePackageError::kNone);

  // This device played SaveA since; the other played SaveB.
  const fs::path to_root = ProfileRoot(to);
  const fs::path from_root = ProfileRoot(from);
  WriteText(to_root / kTitle / "00000001" / "SaveA" / "data.bin", "a here");
  Age(to_root / kTitle / "00000001" / "SaveA" / "data.bin", 3600);
  WriteText(to_root / kTitle / "00000001" / "SaveB" / "extra.bin", "old");
  WriteText(from_root / kTitle / "00000001" / "SaveB" / "data.bin", "b there");
  Age(from_root / kTitle / "00000001" / "SaveB" / "data.bin", 7200);
  REQUIRE(ExportProfilePackage(from, kXuid, {}, package) ==
          ProfilePackageError::kNone);

  ProfileImportResult result;
  REQUIRE(ImportProfilePackage(to, package, options, {}, &result) ==
          ProfilePackageError::kNone);
  CHECK_FALSE(result.created);
  CHECK(ReadText(to_root / kTitle / "00000001" / "SaveA" / "data.bin") ==
        "a here");
  CHECK(ReadText(to_root / kTitle / "00000001" / "SaveB" / "data.bin") ==
        "b there");
  // A saved game moves as a whole: the old one's extra file went with it.
  CHECK_FALSE(
      fs::exists(to_root / kTitle / "00000001" / "SaveB" / "extra.bin"));
  CHECK(result.replaced == 1);
  REQUIRE_FALSE(result.backup_folder.empty());
  const fs::path backup = result.backup_folder / fmt::format("{:016X}", kXuid) /
                          kTitle / "00000001";
  CHECK(ReadText(backup / "SaveB" / "data.bin") == "save b");
  CHECK(ReadText(backup / "SaveB" / "extra.bin") == "old");
  CHECK(result.kept >= 1);

  // Replace takes the package's even where this device's is newer.
  options.mode = ProfileImportMode::kReplace;
  REQUIRE(ImportProfilePackage(to, package, options) ==
          ProfilePackageError::kNone);
  CHECK(ReadText(to_root / kTitle / "00000001" / "SaveA" / "data.bin") ==
        "save a");
}

TEST_CASE("Profile package imports find the same online account",
          "[profile_package]") {
  TempFolder temp;
  const fs::path from = temp.path / "a" / "content";
  const fs::path to = temp.path / "b" / "content";
  MakeProfile(from, "Bo");
  const fs::path package = temp.path / "Bo.xeniosprofile";
  REQUIRE(ExportProfilePackage(from, kXuid, {}, package) ==
          ProfilePackageError::kNone);
  ProfilePackageManifest manifest;
  REQUIRE(InspectProfilePackage(package, &manifest) ==
          ProfilePackageError::kNone);

  // This device joined the account with a link code: its own XUID, the same
  // online one.
  constexpr uint64_t kLinked = 0xE030000000000777ULL;
  {
    X_XAMACCOUNTINFO account = {};
    account.ToggleLiveFlag(true);
    account.xuid_online = kOnlineXuid;
    std::vector<uint8_t> encrypted(sizeof(X_XAMACCOUNTINFO) + 0x18);
    ProfileManager::EncryptAccountFile(&account, encrypted.data());
    const fs::path folder = ProfilePackageProfileFolder(to, kLinked);
    fs::create_directories(folder);
    REQUIRE(xe::filesystem::WriteAllBytes(folder / "Account", encrypted));
  }
  REQUIRE(FindProfileForPackage(to, manifest) == kLinked);

  ProfileImportOptions options;
  options.target_xuid = kLinked;
  options.backup_root = temp.path / "backups";
  ProfileImportResult result;
  REQUIRE(ImportProfilePackage(to, package, options, {}, &result) ==
          ProfilePackageError::kNone);
  CHECK(result.xuid == kLinked);
  const fs::path linked_root = to / fmt::format("{:016X}", kLinked);
  CHECK(ReadText(linked_root / kTitle / "00000001" / "SaveA" / "data.bin") ==
        "save a");
  CHECK(fs::exists(ProfilePackageProfileFolder(to, kLinked) /
                   (std::string(kTitle) + ".gpd")));
  CHECK_FALSE(fs::exists(ProfileRoot(to)));
}

TEST_CASE("Profile package imports refuse what they must",
          "[profile_package]") {
  TempFolder temp;
  const fs::path content = temp.path / "content";
  const fs::path from = temp.path / "a" / "content";
  MakeProfile(from, "Bo");
  const fs::path good = temp.path / "good.xeniosprofile";
  REQUIRE(ExportProfilePackage(from, kXuid, {}, good) ==
          ProfilePackageError::kNone);
  ProfileImportOptions options;
  options.backup_root = temp.path / "backups";

  SECTION("a profile in a game") {
    options.profile_in_use = [](uint64_t xuid) { return xuid == kXuid; };
    CHECK(ImportProfilePackage(content, good, options) ==
          ProfilePackageError::kProfileInUse);
    CHECK_FALSE(fs::exists(ProfileRoot(content)));
  }

  SECTION("a file from a newer app") {
    const fs::path path = temp.path / "newer.xeniosprofile";
    REQUIRE(xe::filesystem::WriteAllBytes(path, Header(2, "{}")));
    CHECK(InspectProfilePackage(path, nullptr) ==
          ProfilePackageError::kNewerFormat);
  }

  SECTION("not a profile file") {
    const fs::path path = temp.path / "junk.xeniosprofile";
    WriteText(path, "this is not a profile at all");
    CHECK(InspectProfilePackage(path, nullptr) ==
          ProfilePackageError::kBadFile);
    CHECK(InspectProfilePackage(temp.path / "missing", nullptr) ==
          ProfilePackageError::kOpenFailed);
  }

  SECTION("paths outside the profile") {
    for (const char* bad :
         {"4D5307E6/00000001/../../../../etc/x", "../outside",
          "FFFE07D1/00010000/E030000000000001/Account", "/tmp/x"}) {
      const fs::path path = temp.path / "bad.xeniosprofile";
      REQUIRE(xe::filesystem::WriteAllBytes(path,
                                            Header(1, ManifestWithPath(bad))));
      CHECK(ImportProfilePackage(content, path, options) ==
            ProfilePackageError::kBadFile);
    }
    CHECK_FALSE(fs::exists(temp.path / "outside"));
    CHECK_FALSE(fs::exists(content));
  }

  SECTION("a manifest whose total isn't its files'") {
    const fs::path path = temp.path / "total.xeniosprofile";
    std::string manifest = ManifestWithPath("4D5307E6/00000001/S/x");
    const auto at = manifest.find("\"total_bytes\":1");
    REQUIRE(at != std::string::npos);
    manifest.replace(at, 15, "\"total_bytes\":0");
    REQUIRE(xe::filesystem::WriteAllBytes(path, Header(1, manifest)));
    CHECK(InspectProfilePackage(path, nullptr) ==
          ProfilePackageError::kBadFile);
  }

  SECTION("a cut-off file") {
    auto data = xe::filesystem::ReadAllBytes(good);
    REQUIRE(data.size() > 40);
    data.resize(data.size() - 20);
    const fs::path path = temp.path / "cut.xeniosprofile";
    REQUIRE(xe::filesystem::WriteAllBytes(path, data));
    CHECK(ImportProfilePackage(content, path, options) ==
          ProfilePackageError::kBadFile);
    CHECK_FALSE(fs::exists(ProfileRoot(content)));
  }

  SECTION("a damaged file, which changes nothing here") {
    // The profile is here already, played since.
    REQUIRE(ImportProfilePackage(content, good, options) ==
            ProfilePackageError::kNone);
    const fs::path save =
        ProfileRoot(content) / kTitle / "00000001" / "SaveA" / "data.bin";
    WriteText(save, "played here");
    auto data = xe::filesystem::ReadAllBytes(good);
    data[data.size() - 30] ^= 0x55;
    const fs::path path = temp.path / "damaged.xeniosprofile";
    REQUIRE(xe::filesystem::WriteAllBytes(path, data));
    options.mode = ProfileImportMode::kReplace;
    CHECK(ImportProfilePackage(content, path, options) ==
          ProfilePackageError::kBadFile);
    CHECK(ReadText(save) == "played here");
    CHECK_FALSE(fs::exists(options.backup_root));
    // Nothing is left of the staging folder.
    for (const auto& entry : fs::directory_iterator(temp.path)) {
      CHECK(entry.path().filename().string().find("staging") ==
            std::string::npos);
    }
  }

  SECTION("a manifest that isn't its Account's profile") {
    auto data = xe::filesystem::ReadAllBytes(good);
    const std::string from_text = "\"gamertag\":\"Bo\"";
    const std::string to_text = "\"gamertag\":\"Al\"";
    auto at = std::search(data.begin(), data.end(), from_text.begin(),
                          from_text.end());
    REQUIRE(at != data.end());
    std::copy(to_text.begin(), to_text.end(), at);
    const fs::path path = temp.path / "renamed.xeniosprofile";
    REQUIRE(xe::filesystem::WriteAllBytes(path, data));
    CHECK(InspectProfilePackage(path, nullptr) ==
          ProfilePackageError::kBadFile);
    CHECK(ImportProfilePackage(content, path, options) ==
          ProfilePackageError::kBadFile);
    CHECK_FALSE(fs::exists(ProfileRoot(content)));
  }

  SECTION("not enough space") {
    options.reserve_bytes = UINT64_MAX / 2;
    CHECK(ImportProfilePackage(content, good, options) ==
          ProfilePackageError::kLowDiskSpace);
  }
}
