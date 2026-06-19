/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_content_management.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <system_error>

#import "xenia/ui/ios/shared/ios_system_utils.h"
#include "xenia/vfs/stfs_metadata.h"

namespace {

constexpr const char* kLinkedContentMarkerSuffix = ".xenia-ios-link";
constexpr const char* kLinkedContentMarkerVersion = "version=1";
constexpr std::string_view kLinkedContentSourcePrefix = "source_path=";
constexpr std::string_view kLinkedContentRelativePrefix = "relative_path=";

std::filesystem::path LinkedContentMarkerPathForPackageDirectory(
    const std::filesystem::path& package_directory) {
  return package_directory.parent_path() /
         (package_directory.filename().string() + kLinkedContentMarkerSuffix);
}

bool ReadLinkedContentMarker(const std::filesystem::path& package_directory,
                             std::filesystem::path* source_path_out,
                             std::filesystem::path* relative_path_out) {
  if (source_path_out) {
    source_path_out->clear();
  }
  if (relative_path_out) {
    relative_path_out->clear();
  }

  std::ifstream marker(LinkedContentMarkerPathForPackageDirectory(package_directory),
                       std::ios::binary);
  if (!marker) {
    return false;
  }

  std::string line;
  std::filesystem::path source_path;
  std::filesystem::path relative_path;
  while (std::getline(marker, line)) {
    if (line.rfind(kLinkedContentSourcePrefix, 0) == 0) {
      source_path = std::filesystem::path(line.substr(kLinkedContentSourcePrefix.size()));
    } else if (line.rfind(kLinkedContentRelativePrefix, 0) == 0) {
      relative_path = std::filesystem::path(line.substr(kLinkedContentRelativePrefix.size()));
    }
  }

  if (source_path.empty()) {
    return false;
  }
  if (source_path_out) {
    *source_path_out = std::move(source_path);
  }
  if (relative_path_out) {
    *relative_path_out = std::move(relative_path);
  }
  return true;
}

bool WriteLinkedContentMarker(const std::filesystem::path& package_directory,
                              const std::filesystem::path& source_path,
                              const std::filesystem::path& relative_path,
                              std::string* error_message_out) {
  std::ofstream marker(LinkedContentMarkerPathForPackageDirectory(package_directory),
                       std::ios::binary | std::ios::trunc);
  if (!marker) {
    if (error_message_out) {
      *error_message_out = "Failed writing linked content marker.";
    }
    return false;
  }

  marker << kLinkedContentMarkerVersion << '\n';
  marker << kLinkedContentSourcePrefix << source_path.string() << '\n';
  marker << kLinkedContentRelativePrefix << relative_path.string() << '\n';
  if (!marker.good()) {
    if (error_message_out) {
      *error_message_out = "Failed writing linked content marker.";
    }
    return false;
  }
  return true;
}

bool LinkFileReplacing(const std::filesystem::path& source,
                       const std::filesystem::path& destination, std::string* error_message_out) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(source, ec) || ec) {
    if (error_message_out) {
      *error_message_out = "Linked content source file is unavailable.";
    }
    return false;
  }

  ec.clear();
  std::filesystem::remove(destination, ec);
  ec.clear();
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (error_message_out) {
      *error_message_out = "Failed creating linked content folder: " + ec.message();
    }
    return false;
  }

  ec.clear();
  std::filesystem::create_symlink(source, destination, ec);
  if (ec) {
    if (error_message_out) {
      *error_message_out = "Failed linking content file: " + ec.message();
    }
    return false;
  }
  return true;
}

bool LinkDirectoryFilesRecursive(const std::filesystem::path& source,
                                 const std::filesystem::path& destination,
                                 std::string* error_message_out) {
  std::error_code ec;
  std::filesystem::remove_all(destination, ec);
  ec.clear();
  std::filesystem::create_directories(destination, ec);
  if (ec) {
    if (error_message_out) {
      *error_message_out = "Failed creating linked content data folder: " + ec.message();
    }
    return false;
  }

  std::filesystem::recursive_directory_iterator it(source, ec);
  std::filesystem::recursive_directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    const std::filesystem::path source_entry = it->path();
    const std::filesystem::path relative_path = std::filesystem::relative(source_entry, source, ec);
    if (ec) {
      break;
    }
    const std::filesystem::path destination_entry = destination / relative_path;

    const std::filesystem::file_status link_status = it->symlink_status(ec);
    if (ec) {
      break;
    }
    if (std::filesystem::is_symlink(link_status)) {
      if (error_message_out) {
        *error_message_out = "Linked content data folder contains an unsupported symlink.";
      }
      return false;
    }
    if (std::filesystem::is_directory(link_status)) {
      std::filesystem::create_directories(destination_entry, ec);
      if (ec) {
        break;
      }
      continue;
    }
    if (std::filesystem::is_regular_file(link_status)) {
      if (!LinkFileReplacing(source_entry, destination_entry, error_message_out)) {
        return false;
      }
      continue;
    }

    if (error_message_out) {
      *error_message_out = "Linked content data folder contains an unsupported file type.";
    }
    return false;
  }

  if (ec) {
    if (error_message_out) {
      *error_message_out = "Failed linking content data folder: " + ec.message();
    }
    return false;
  }
  return true;
}

bool RebuildLinkedContentProjection(const std::filesystem::path& package_directory,
                                    const std::filesystem::path& source_package_path,
                                    std::string* error_message_out) {
  if (error_message_out) {
    *error_message_out = "";
  }

  std::error_code ec;
  std::filesystem::remove_all(package_directory, ec);
  ec.clear();
  std::filesystem::create_directories(package_directory, ec);
  if (ec) {
    if (error_message_out) {
      *error_message_out = "Failed creating linked content folder: " + ec.message();
    }
    return false;
  }

  const std::filesystem::path destination_file = package_directory / source_package_path.filename();
  if (!LinkFileReplacing(source_package_path, destination_file, error_message_out)) {
    return false;
  }

  if (HasContentSidecarDataDirectory(source_package_path)) {
    std::filesystem::path source_sidecar = source_package_path;
    source_sidecar += ".data";
    std::filesystem::path destination_sidecar = destination_file;
    destination_sidecar += ".data";
    if (!LinkDirectoryFilesRecursive(source_sidecar, destination_sidecar, error_message_out)) {
      return false;
    }
  }

  return true;
}

}  // namespace

bool HasContentSidecarDataDirectory(const std::filesystem::path& path) {
  std::filesystem::path sidecar_path = path;
  sidecar_path += ".data";
  std::error_code ec;
  return std::filesystem::is_directory(sidecar_path, ec) && !ec;
}

std::filesystem::path xe_title_content_root(uint32_t title_id) {
  char title_id_buffer[9] = {};
  std::snprintf(title_id_buffer, sizeof(title_id_buffer), "%08X", title_id);
  return xe_get_ios_documents_path() / "content" / "0000000000000000" / title_id_buffer;
}

std::filesystem::path xe_title_update_content_root(uint32_t title_id) {
  return xe_title_content_root(title_id) / "000B0000";
}

std::filesystem::path xe_dlc_content_root(uint32_t title_id) {
  return xe_title_content_root(title_id) / "00000002";
}

NSString* xe_installed_content_kind_label(IOSInstalledContentKind kind) {
  switch (kind) {
    case IOSInstalledContentKind::kTitleUpdate:
      return @"Title Update";
    case IOSInstalledContentKind::kDlc:
      return @"DLC";
  }
  return @"Content";
}

std::string xe_content_package_directory_name(const std::filesystem::path& package_path) {
  std::string name = package_path.stem().string();
  if (name.empty()) {
    name = package_path.filename().string();
  }
  return name;
}

bool xe_read_selected_content_package(const std::filesystem::path& path,
                                      IOSSelectedContentPackage* package_out,
                                      NSString** error_message_out) {
  if (error_message_out) {
    *error_message_out = nil;
  }

  auto metadata = xe::vfs::ExtractStfsMetadata(path);
  if (!metadata.has_value()) {
    if (error_message_out) {
      *error_message_out = @"Could not read the content package header.";
    }
    return false;
  }

  if (metadata->data_file_count > 0 && !HasContentSidecarDataDirectory(path)) {
    if (error_message_out) {
      *error_message_out = @"This content package is missing its required .data sidecar folder.";
    }
    return false;
  }

  if (package_out) {
    package_out->title_id = metadata->title_id;
    package_out->content_type = static_cast<xe::XContentType>(metadata->content_type);
    package_out->path = path;
  }
  return true;
}

bool xe_copy_directory_recursive(const std::filesystem::path& source,
                                 const std::filesystem::path& destination,
                                 std::string* error_message_out) {
  std::error_code ec;
  std::filesystem::remove_all(destination, ec);
  ec.clear();
  std::filesystem::copy(
      source, destination,
      std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
      ec);
  if (ec) {
    if (error_message_out) {
      *error_message_out = ec.message();
    }
    return false;
  }
  return true;
}

bool xe_copy_content_package_into_root(const IOSSelectedContentPackage& package_info,
                                       const std::filesystem::path& destination_root,
                                       std::string* error_message_out) {
  if (error_message_out) {
    *error_message_out = "";
  }

  const std::filesystem::path package_directory =
      destination_root / xe_content_package_directory_name(package_info.path);
  std::error_code ec;
  std::filesystem::create_directories(package_directory, ec);
  if (ec) {
    if (error_message_out) {
      *error_message_out = "Failed creating content folder: " + ec.message();
    }
    return false;
  }

  const std::filesystem::path destination_file = package_directory / package_info.path.filename();
  std::filesystem::copy_file(package_info.path, destination_file,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    if (error_message_out) {
      *error_message_out = "Failed copying package: " + ec.message();
    }
    return false;
  }

  if (HasContentSidecarDataDirectory(package_info.path)) {
    std::filesystem::path source_sidecar = package_info.path;
    source_sidecar += ".data";
    std::filesystem::path destination_sidecar = destination_file;
    destination_sidecar += ".data";
    if (!xe_copy_directory_recursive(source_sidecar, destination_sidecar, error_message_out)) {
      return false;
    }
  }

  return true;
}

bool xe_link_content_package_into_root(const IOSSelectedContentPackage& package_info,
                                       const std::filesystem::path& destination_root,
                                       const std::filesystem::path& source_relative_path,
                                       std::string* error_message_out) {
  if (error_message_out) {
    *error_message_out = "";
  }

  const std::filesystem::path package_directory =
      destination_root / xe_content_package_directory_name(package_info.path);
  const std::filesystem::path marker_path =
      LinkedContentMarkerPathForPackageDirectory(package_directory);
  std::error_code ec;
  std::filesystem::remove(marker_path, ec);

  if (!RebuildLinkedContentProjection(package_directory, package_info.path, error_message_out)) {
    std::filesystem::remove_all(package_directory, ec);
    return false;
  }

  if (!WriteLinkedContentMarker(package_directory, package_info.path, source_relative_path,
                                error_message_out)) {
    std::filesystem::remove_all(package_directory, ec);
    std::filesystem::remove(marker_path, ec);
    return false;
  }

  return true;
}

bool xe_refresh_linked_content_entry(const IOSInstalledContentEntry& entry,
                                     const std::filesystem::path& current_source_path,
                                     std::string* error_message_out) {
  if (!entry.linked) {
    if (error_message_out) {
      *error_message_out = "Content entry is not linked.";
    }
    return false;
  }

  if (!RebuildLinkedContentProjection(entry.path, current_source_path, error_message_out)) {
    return false;
  }

  return WriteLinkedContentMarker(entry.path, current_source_path, entry.linked_relative_path,
                                  error_message_out);
}

void xe_remove_linked_content_marker(const IOSInstalledContentEntry& entry) {
  std::error_code ec;
  std::filesystem::remove(LinkedContentMarkerPathForPackageDirectory(entry.path), ec);
}

void xe_collect_installed_content(const std::filesystem::path& root, IOSInstalledContentKind kind,
                                  std::vector<IOSInstalledContentEntry>* content_out) {
  if (!content_out) {
    return;
  }

  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    return;
  }

  std::filesystem::directory_iterator it(root, ec);
  std::filesystem::directory_iterator end;
  for (; !ec && it != end; ++it) {
    if (!it->is_directory(ec) || ec) {
      ec.clear();
      continue;
    }

    IOSInstalledContentEntry entry;
    entry.kind = kind;
    entry.name = it->path().filename().string();
    entry.path = it->path();
    entry.linked =
        ReadLinkedContentMarker(entry.path, &entry.linked_source_path, &entry.linked_relative_path);
    content_out->push_back(std::move(entry));
  }
}

std::vector<IOSInstalledContentEntry> xe_list_installed_content(uint32_t title_id) {
  std::vector<IOSInstalledContentEntry> content;
  if (!title_id) {
    return content;
  }

  xe_collect_installed_content(xe_title_update_content_root(title_id),
                               IOSInstalledContentKind::kTitleUpdate, &content);
  xe_collect_installed_content(xe_dlc_content_root(title_id), IOSInstalledContentKind::kDlc,
                               &content);
  std::sort(content.begin(), content.end(),
            [](const IOSInstalledContentEntry& a, const IOSInstalledContentEntry& b) {
              if (a.kind != b.kind) {
                return a.kind < b.kind;
              }
              return a.name < b.name;
            });
  return content;
}

std::vector<IOSInstalledContentEntry> xe_list_linked_content(uint32_t title_id) {
  std::vector<IOSInstalledContentEntry> linked_content;
  for (IOSInstalledContentEntry& entry : xe_list_installed_content(title_id)) {
    if (entry.linked) {
      linked_content.push_back(std::move(entry));
    }
  }
  return linked_content;
}
