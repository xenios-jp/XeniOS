/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/touch/touch_layout_store_ios.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace xe {
namespace ui {

namespace {

std::filesystem::path BackupPathForLayoutPath(const std::filesystem::path& path) {
  return path.string() + ".bak";
}

bool WriteTextFileAtomically(const std::filesystem::path& path, const std::string& payload,
                             NSString** error_out) {
  std::filesystem::path tmp_path = path.string() + ".tmp";
  std::filesystem::path backup_path = BackupPathForLayoutPath(path);
  const std::string tmp_path_utf8 = xe::path_to_utf8(tmp_path);
  int fd = ::open(tmp_path_utf8.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    if (error_out) {
      *error_out = [NSString
          stringWithFormat:NSLocalizedString(
                               @"Could not open temporary layout file: %s",
                               @"Error shown when saving a touch layout; %s is the system error"),
                           std::strerror(errno)];
    }
    return false;
  }

  const char* bytes = payload.data();
  size_t remaining = payload.size();
  while (remaining > 0) {
    ssize_t written = ::write(fd, bytes, remaining);
    if (written < 0) {
      NSString* message = [NSString
          stringWithFormat:NSLocalizedString(
                               @"Could not write layout file: %s",
                               @"Error shown when saving a touch layout; %s is the system error"),
                           std::strerror(errno)];
      ::close(fd);
      std::error_code remove_ec;
      std::filesystem::remove(tmp_path, remove_ec);
      if (error_out) {
        *error_out = message;
      }
      return false;
    }
    bytes += written;
    remaining -= static_cast<size_t>(written);
  }

  if (::fsync(fd) != 0) {
    NSString* message = [NSString
        stringWithFormat:NSLocalizedString(
                             @"Could not sync layout file: %s",
                             @"Error shown when saving a touch layout; %s is the system error"),
                         std::strerror(errno)];
    ::close(fd);
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    if (error_out) {
      *error_out = message;
    }
    return false;
  }
  if (::close(fd) != 0) {
    if (error_out) {
      *error_out = [NSString
          stringWithFormat:NSLocalizedString(
                               @"Could not close layout file: %s",
                               @"Error shown when saving a touch layout; %s is the system error"),
                           std::strerror(errno)];
    }
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    return false;
  }

  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    std::filesystem::copy_file(path, backup_path,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      XELOGW("iOS: failed to update touch layout backup {}: {}",
             xe::strip_pii(xe::path_to_utf8(backup_path)), ec.message());
      ec.clear();
    }
  }
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    const std::string rename_error = ec.message();
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    if (error_out) {
      *error_out = [NSString
          stringWithFormat:NSLocalizedString(
                               @"Could not replace layout file: %s",
                               @"Error shown when saving a touch layout; %s is the system error"),
                           rename_error.c_str()];
    }
    return false;
  }
  return true;
}

bool LoadIOSTouchLayoutModelAtPathInternal(
    const std::filesystem::path& layout_path,
    xe::hid::touch::IOSTouchLayoutModel* layout_out, NSString** error_out) {
  toml::table table = toml::parse_file(xe::path_to_utf8(layout_path));
  xe::hid::touch::IOSTouchLayoutModel layout = MakeTouchLayoutSeedModelForTable(table);
  if (!xe::hid::touch::ApplyIOSTouchLayoutModel(table, &layout)) {
    if (error_out) {
      *error_out = NSLocalizedString(@"Touch layout file could not be applied.",
                                     @"Error shown when loading a touch layout");
    }
    return false;
  }
  *layout_out = std::move(layout);
  return true;
}

}  // namespace

std::filesystem::path IOSTouchLayoutsDirectory() {
  return xe_get_ios_documents_path() / "touch-layouts";
}

std::filesystem::path IOSTouchLayoutPathForLocalID(
    const std::string& local_id) {
  return IOSTouchLayoutsDirectory() / (local_id + ".toml");
}

bool EnsureIOSTouchLayoutsDirectory(NSString** error_out) {
  std::error_code ec;
  std::filesystem::create_directories(IOSTouchLayoutsDirectory(), ec);
  if (ec) {
    if (error_out) {
      *error_out = [NSString
          stringWithFormat:NSLocalizedString(
                               @"Could not create layout folder: %s",
                               @"Error shown when saving a touch layout; %s is the system error"),
                           ec.message().c_str()];
    }
    XELOGE("iOS: failed to create touch layout directory: {}", ec.message());
    return false;
  }
  return true;
}

bool WriteIOSTouchLayoutModel(
    const xe::hid::touch::IOSTouchLayoutModel& layout,
    const std::filesystem::path& layout_path, NSString** error_out) {
  std::error_code ec;
  std::filesystem::create_directories(layout_path.parent_path(), ec);
  if (ec) {
    if (error_out) {
      *error_out = [NSString
          stringWithFormat:NSLocalizedString(
                               @"Could not create layout folder: %s",
                               @"Error shown when saving a touch layout; %s is the system error"),
                           ec.message().c_str()];
    }
    return false;
  }

  std::ostringstream payload_stream;
  payload_stream << "# XeniOS touch layout\n\n";
  payload_stream << xe::hid::touch::EncodeIOSTouchLayoutModel(layout) << "\n";
  if (!WriteTextFileAtomically(layout_path, payload_stream.str(), error_out)) {
    XELOGE("iOS: failed to write touch layout {}",
           xe::strip_pii(xe::path_to_utf8(layout_path)));
    return false;
  }
  return true;
}

bool LoadIOSTouchLayoutModelAtPath(
    const std::filesystem::path& layout_path,
    xe::hid::touch::IOSTouchLayoutModel* layout_out, NSString** error_out) {
  if (!layout_out) {
    return false;
  }

  try {
    if (!LoadIOSTouchLayoutModelAtPathInternal(layout_path, layout_out, error_out)) {
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    std::filesystem::path backup_path = BackupPathForLayoutPath(layout_path);
    std::error_code ec;
    if (std::filesystem::exists(backup_path, ec)) {
      try {
        if (LoadIOSTouchLayoutModelAtPathInternal(backup_path, layout_out, error_out)) {
          XELOGW("iOS: recovered touch layout {} from backup after parse failure: {}",
                 xe::strip_pii(xe::path_to_utf8(layout_path)), e.what());
          return true;
        }
      } catch (const std::exception& backup_e) {
        XELOGE("iOS: failed to recover touch layout backup {}: {}",
               xe::strip_pii(xe::path_to_utf8(backup_path)), backup_e.what());
      }
    }
    if (error_out) {
      *error_out = ToNSString(e.what());
    }
    XELOGE("iOS: failed to read touch layout {}: {}",
           xe::strip_pii(xe::path_to_utf8(layout_path)), e.what());
    return false;
  }
}

void EnsureOfficialIOSTouchLayoutPresets() {
  if (!EnsureIOSTouchLayoutsDirectory()) {
    return;
  }
  const char* official_layout_ids[] = {
      kOfficialTouchLayoutFPSCompactLocalID,
      kOfficialTouchLayoutFPSFullLocalID,
      kOfficialTouchLayoutActionAdventureLocalID,
      kOfficialTouchLayoutArcadeDpadLocalID,
      kOfficialTouchLayoutDrivingLocalID,
      kOfficialTouchLayoutMinimalStarterLocalID,
  };
  const char* obsolete_official_layout_ids[] = {
      "fps_standard",
      "fps_ipad",
      "fps_expanded",
      "fps_mirrored",
  };
  for (const char* local_id : obsolete_official_layout_ids) {
    std::error_code remove_error;
    std::filesystem::remove(IOSTouchLayoutPathForLocalID(local_id),
                            remove_error);
  }
  for (const char* local_id : official_layout_ids) {
    NSString* error_message = nil;
    if (!WriteIOSTouchLayoutModel(
            MakeOfficialIOSTouchLayoutModelForLocalID(local_id),
            IOSTouchLayoutPathForLocalID(local_id), &error_message)) {
      XELOGE("iOS: failed to refresh official touch layout {}: {}", local_id,
             error_message ? error_message.UTF8String : "unknown error");
    }
  }
}

std::vector<IOSTouchLocalLayoutInfo> AvailableLocalIOSTouchLayouts() {
  EnsureOfficialIOSTouchLayoutPresets();

  std::vector<IOSTouchLocalLayoutInfo> layouts;
  std::error_code ec;
  const std::filesystem::path layouts_directory = IOSTouchLayoutsDirectory();
  if (!std::filesystem::exists(layouts_directory, ec)) {
    return layouts;
  }

  for (const auto& entry :
       std::filesystem::directory_iterator(layouts_directory, ec)) {
    if (ec || !entry.is_regular_file()) {
      continue;
    }
    if (entry.path().extension() != ".toml") {
      continue;
    }

    const std::string stem = entry.path().stem().string();
    std::string normalized_stem;
    if (!TryNormalizeConfiguredTouchLayoutLocalID(stem, &normalized_stem) ||
        normalized_stem != stem) {
      XELOGW("iOS: skipping unsafe touch layout filename {}", stem);
      continue;
    }

    xe::hid::touch::IOSTouchLayoutModel layout;
    NSString* error_message = nil;
    if (!LoadIOSTouchLayoutModelAtPath(entry.path(), &layout, &error_message)) {
      continue;
    }

    IOSTouchLocalLayoutInfo info;
    info.path = entry.path();
    info.local_id = normalized_stem;
    info.display_name =
        layout.display_name.empty() ? info.local_id : layout.display_name;
    info.author = layout.author;
    info.official = IsOfficialTouchLayoutLocalID(info.local_id);
    info.layout = std::move(layout);
    layouts.push_back(std::move(info));
  }

  std::sort(layouts.begin(), layouts.end(),
            [](const IOSTouchLocalLayoutInfo& left,
               const IOSTouchLocalLayoutInfo& right) {
              if (left.official != right.official) {
                return left.official;
              }
              if (left.official && right.official) {
                return OfficialTouchLayoutPresetSortOrder(left.local_id) <
                       OfficialTouchLayoutPresetSortOrder(right.local_id);
              }
              return left.display_name < right.display_name;
            });
  return layouts;
}

std::string UniqueIOSTouchLayoutLocalIDForBaseName(
    NSString* base_name, const std::string& existing_local_id) {
  std::string base =
      MakeTouchLayoutSlug(base_name ? std::string([base_name UTF8String])
                                    : std::string());
  if (IsOfficialTouchLayoutLocalID(base)) {
    base += "_copy";
  }

  std::string local_id = base;
  int duplicate_index = 2;
  while (local_id != existing_local_id &&
         std::filesystem::exists(IOSTouchLayoutPathForLocalID(local_id))) {
    local_id = fmt::format("{}_{}", base, duplicate_index++);
  }
  return local_id;
}

}  // namespace ui
}  // namespace xe
