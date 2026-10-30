/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_CONTENT_MANAGEMENT_H_
#define XENIA_UI_IOS_CONTENT_MANAGEMENT_H_

#import <UIKit/UIKit.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xenia/xbox.h"

// Title-update / DLC discovery and installation utilities.
//
// On disk content lives under
//   {documents}/content/0000000000000000/{title_id_hex_upper}/{type_id}/{pkg}/
// where type_id is "000B0000" for title updates and "00000002" for DLC. This
// module enumerates installed content packages, copies new ones into place
// (preserving headers + .data sidecar dirs) and reads top-level metadata.

enum class IOSInstalledContentKind {
  kTitleUpdate,
  kDlc,
};

struct IOSInstalledContentEntry {
  IOSInstalledContentKind kind = IOSInstalledContentKind::kTitleUpdate;
  std::string name;
  std::filesystem::path path;
  bool linked = false;
  std::filesystem::path linked_source_path;
  std::filesystem::path linked_relative_path;
};

struct IOSSelectedContentPackage {
  uint32_t title_id = 0;
  xe::XContentType content_type = xe::XContentType::kSavedGame;
  std::filesystem::path path;
};

// True when {path}.data exists and is a directory. Many .gpd / xex container
// packages keep their data in a sibling directory; this helper centralises
// the check used by both the content installer and the importer.
bool HasContentSidecarDataDirectory(const std::filesystem::path& path);

// Disk locations.
std::filesystem::path xe_title_content_root(uint32_t title_id);
std::filesystem::path xe_title_update_content_root(uint32_t title_id);
std::filesystem::path xe_dlc_content_root(uint32_t title_id);

// User-visible labels.
NSString* xe_installed_content_kind_label(IOSInstalledContentKind kind);

// Package metadata + I/O.
std::string xe_content_package_directory_name(const std::filesystem::path& package_path);
bool xe_read_selected_content_package(const std::filesystem::path& path,
                                      IOSSelectedContentPackage* package_out,
                                      NSString** error_message_out);
bool xe_copy_directory_recursive(const std::filesystem::path& source,
                                 const std::filesystem::path& destination,
                                 std::string* error_message_out);
bool xe_copy_content_package_into_root(const IOSSelectedContentPackage& package_info,
                                       const std::filesystem::path& destination_root,
                                       std::string* error_message_out);
bool xe_link_content_package_into_root(const IOSSelectedContentPackage& package_info,
                                       const std::filesystem::path& destination_root,
                                       const std::filesystem::path& source_relative_path,
                                       std::string* error_message_out);
bool xe_refresh_linked_content_entry(const IOSInstalledContentEntry& entry,
                                     const std::filesystem::path& current_source_path,
                                     std::string* error_message_out);
void xe_remove_linked_content_marker(const IOSInstalledContentEntry& entry);

// Enumerators.
void xe_collect_installed_content(const std::filesystem::path& root, IOSInstalledContentKind kind,
                                  std::vector<IOSInstalledContentEntry>* content_out);
std::vector<IOSInstalledContentEntry> xe_list_installed_content(uint32_t title_id);
std::vector<IOSInstalledContentEntry> xe_list_linked_content(uint32_t title_id);

#endif  // XENIA_UI_IOS_CONTENT_MANAGEMENT_H_
