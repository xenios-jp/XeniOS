/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_content_management.h"

#include "xenia/ui/ios/shared/ios_edge_adapter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string_view>
#include <system_error>

#include "xenia/base/logging.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#include "xenia/vfs/stfs_metadata.h"

namespace {

constexpr const char* kLinkedContentMarkerSuffix = ".xenia-ios-link";
// Where the DLC layout migration sets a package's old folder while it works.
constexpr const char* kMigrateAsideSuffix = ".xenios-migrate";
constexpr const char* kLinkedContentMarkerVersion = "version=1";
constexpr std::string_view kLinkedContentSourcePrefix = "source_path=";
constexpr std::string_view kLinkedContentRelativePrefix = "relative_path=";

// The marker sits beside the package, named after it.
std::filesystem::path LinkedContentMarkerPathForPackage(const std::filesystem::path& package_path) {
  return package_path.parent_path() /
         (package_path.filename().string() + kLinkedContentMarkerSuffix);
}

std::filesystem::path ContentSidecarPath(const std::filesystem::path& package_path) {
  std::filesystem::path sidecar_path = package_path;
  sidecar_path += ".data";
  return sidecar_path;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool ReadLinkedContentMarker(const std::filesystem::path& package_path,
                             std::filesystem::path* source_path_out,
                             std::filesystem::path* relative_path_out) {
  if (source_path_out) {
    source_path_out->clear();
  }
  if (relative_path_out) {
    relative_path_out->clear();
  }

  std::ifstream marker(LinkedContentMarkerPathForPackage(package_path), std::ios::binary);
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

bool WriteLinkedContentMarker(const std::filesystem::path& package_path,
                              const std::filesystem::path& source_path,
                              const std::filesystem::path& relative_path,
                              std::string* error_message_out) {
  std::ofstream marker(LinkedContentMarkerPathForPackage(package_path),
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

// Links the package file itself at |package_path| (and its ".data" sidecar
// beside it), which is how the content manager finds a package kept as a file.
bool RebuildLinkedContentProjection(const std::filesystem::path& package_path,
                                    const std::filesystem::path& source_package_path,
                                    std::string* error_message_out) {
  if (error_message_out) {
    *error_message_out = "";
  }

  std::error_code ec;
  std::filesystem::remove_all(package_path, ec);
  ec.clear();
  std::filesystem::remove_all(ContentSidecarPath(package_path), ec);

  if (!LinkFileReplacing(source_package_path, package_path, error_message_out)) {
    return false;
  }

  if (HasContentSidecarDataDirectory(source_package_path)) {
    if (!LinkDirectoryFilesRecursive(ContentSidecarPath(source_package_path),
                                     ContentSidecarPath(package_path), error_message_out)) {
      return false;
    }
  }

  return true;
}

// XeniOS 2.3.1 and older installed DLC as {root}/{stem}/{file} (plus
// {file}.data). The content manager mounts that folder as an extracted package
// holding one file, so titles found no DLC in it. Moves such a package up to
// {root}/{file}. Only touches a folder holding exactly that one package file
// and its sidecar.
void MigrateNestedDlcPackage(const std::filesystem::path& root,
                             const std::filesystem::path& folder) {
  std::error_code ec;
  std::filesystem::path package_name;
  bool has_sidecar = false;
  size_t child_count = 0;
  std::filesystem::directory_iterator it(folder, ec);
  std::filesystem::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    ++child_count;
    std::error_code type_ec;
    const auto status = it->symlink_status(type_ec);
    if (type_ec) {
      return;
    }
    const std::string name = it->path().filename().string();
    if (std::filesystem::is_directory(status) && EndsWith(name, ".data")) {
      has_sidecar = true;
    } else if (std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
      if (!package_name.empty()) {
        return;
      }
      package_name = it->path().filename();
    } else {
      return;
    }
  }
  if (ec || package_name.empty() || child_count != (has_sidecar ? 2u : 1u) ||
      EndsWith(package_name.string(), ".part") ||
      xe_content_package_directory_name(package_name) != folder.filename().string()) {
    return;
  }
  if (has_sidecar &&
      !std::filesystem::is_directory(ContentSidecarPath(folder / package_name), ec)) {
    return;
  }
  const std::filesystem::path target = root / package_name;
  if (has_sidecar &&
      std::filesystem::exists(std::filesystem::symlink_status(ContentSidecarPath(target), ec))) {
    return;
  }
  const std::filesystem::path old_marker = LinkedContentMarkerPathForPackage(folder);
  const bool linked = std::filesystem::exists(old_marker, ec);
  // A linked package's source can be outside the sandbox scope during the
  // scan, so trust the marker for it.
  if (!linked && !xe::vfs::ExtractStfsMetadata(folder / package_name).has_value()) {
    return;
  }

  std::filesystem::path source_folder = folder;
  if (target == folder) {
    // The usual case: package files have no extension, so the folder already
    // has the package's name.
    source_folder = root / (folder.filename().string() + kMigrateAsideSuffix);
    std::filesystem::rename(folder, source_folder, ec);
    if (ec) {
      XELOGW("DLC layout migration: could not move aside {}: {}", xe::strip_pii(folder.string()),
             ec.message());
      return;
    }
  } else if (std::filesystem::exists(std::filesystem::symlink_status(target, ec))) {
    return;
  }

  std::filesystem::rename(source_folder / package_name, target, ec);
  if (ec) {
    XELOGW("DLC layout migration: could not move {}: {}",
           xe::strip_pii((source_folder / package_name).string()), ec.message());
    if (source_folder != folder) {
      std::error_code undo_ec;
      std::filesystem::rename(source_folder, folder, undo_ec);
    }
    return;
  }
  if (has_sidecar) {
    std::filesystem::rename(ContentSidecarPath(source_folder / package_name),
                            ContentSidecarPath(target), ec);
    if (ec) {
      XELOGW("DLC layout migration: could not move the data of {}: {}",
             xe::strip_pii(target.string()), ec.message());
    }
  }
  if (linked) {
    std::error_code marker_ec;
    std::filesystem::rename(old_marker, LinkedContentMarkerPathForPackage(target), marker_ec);
  }
  std::error_code remove_ec;
  std::filesystem::remove(source_folder, remove_ec);
  XELOGI("DLC layout migration: moved {} out of its folder", xe::strip_pii(target.string()));
}

// A migration stopped part way (the app was closed) leaves its folder set
// aside: put it back if the package hadn't moved yet, else move what's left.
void ResumeNestedDlcMigration(const std::filesystem::path& root,
                              const std::filesystem::path& aside) {
  std::string name = aside.filename().string();
  name.resize(name.size() - std::strlen(kMigrateAsideSuffix));
  const std::filesystem::path target = root / name;
  std::error_code ec;
  const auto target_status = std::filesystem::symlink_status(target, ec);
  if (!std::filesystem::exists(target_status)) {
    std::filesystem::rename(aside, target, ec);
    if (!ec) {
      MigrateNestedDlcPackage(root, target);
    }
    return;
  }
  if (std::filesystem::is_directory(target_status)) {
    return;
  }
  const std::filesystem::path sidecar = ContentSidecarPath(aside / name);
  if (std::filesystem::is_directory(sidecar, ec) &&
      !std::filesystem::exists(std::filesystem::symlink_status(ContentSidecarPath(target), ec))) {
    std::filesystem::rename(sidecar, ContentSidecarPath(target), ec);
  }
  std::filesystem::remove(aside, ec);
  if (ec) {
    XELOGW("DLC layout migration: could not finish {}: {}", xe::strip_pii(target.string()),
           ec.message());
  }
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
      return NSLocalizedString(@"Title Update", @"content management: title");
    case IOSInstalledContentKind::kDlc:
      return NSLocalizedString(@"DLC", @"content management: user-visible text");
  }
  return NSLocalizedString(@"Content", @"content management: user-visible text");
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
      *error_message_out = NSLocalizedString(@"Could not read the content package header.",
                                             @"content management: error message");
    }
    return false;
  }

  if (xe::ui::IOSContentDataFileCount(path) > 0 && !HasContentSidecarDataDirectory(path)) {
    if (error_message_out) {
      *error_message_out =
          NSLocalizedString(@"This content package is missing its required .data sidecar folder.",
                            @"content management: error message");
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
                                       XeniaIOSFileTransfer* transfer,
                                       XeniaIOSFileTransferProgress progress,
                                       std::string* error_message_out) {
  if (error_message_out) {
    *error_message_out = "";
  }

  std::error_code ec;
  std::filesystem::create_directories(destination_root, ec);
  if (ec) {
    if (error_message_out) {
      *error_message_out = "Failed creating content folder: " + ec.message();
    }
    return false;
  }
  XeniaIOSExcludeFromBackup([NSString stringWithUTF8String:destination_root.c_str()]);

  // The package file goes straight into the content type folder: the content
  // manager mounts a file there as the package and a folder as its extracted
  // contents.
  const std::filesystem::path destination_file = destination_root / package_info.path.filename();
  // The package file is first so it is renamed into place last, after its
  // ".data" sidecar folder.
  NSMutableArray<NSArray<NSString*>*>* pairs = [NSMutableArray
      arrayWithObject:@[
        [NSString stringWithUTF8String:package_info.path.c_str()],
        [NSString stringWithUTF8String:destination_file.c_str()]
      ]];
  if (HasContentSidecarDataDirectory(package_info.path)) {
    std::filesystem::path source_sidecar = package_info.path;
    source_sidecar += ".data";
    std::filesystem::path destination_sidecar = destination_file;
    destination_sidecar += ".data";
    [pairs addObject:@[
      [NSString stringWithUTF8String:source_sidecar.c_str()],
      [NSString stringWithUTF8String:destination_sidecar.c_str()]
    ]];
  }

  transfer.replacesExisting = YES;
  NSError* copy_error = nil;
  if (![transfer copyPairs:pairs progress:progress error:&copy_error]) {
    if (error_message_out) {
      *error_message_out = copy_error.localizedDescription
                               ? std::string([copy_error.localizedDescription UTF8String])
                               : std::string("Failed copying package.");
    }
    return false;
  }
  if (transfer.skippedItemCount > 0 && error_message_out) {
    // Success with a warning: callers show it when the text is not empty.
    *error_message_out = "Copied, but " + std::to_string(transfer.skippedItemCount) +
                         " item(s) (links or special files) were skipped.";
  }
  return true;
}

void xe_exclude_installed_content_from_backup() {
  @autoreleasepool {
    const std::filesystem::path user_root =
        xe_get_ios_documents_path() / "content" / "0000000000000000";
    std::error_code ec;
    std::filesystem::directory_iterator it(user_root, ec);
    std::filesystem::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
      if (!it->is_directory(ec)) {
        continue;
      }
      for (const char* type_directory : {"000B0000", "00000002"}) {
        const std::filesystem::path root = it->path() / type_directory;
        if (std::filesystem::is_directory(root, ec)) {
          XeniaIOSExcludeFromBackup([NSString stringWithUTF8String:root.c_str()]);
          if (std::string(type_directory) == "00000002") {
            // Partial DLC copies left by a killed app; live ones are registered and skipped.
            XeniaIOSSweepStalePartFiles(root);
            std::error_code package_ec;
            std::filesystem::directory_iterator package_it(root, package_ec);
            for (; !package_ec && package_it != end; package_it.increment(package_ec)) {
              std::error_code dir_ec;
              if (package_it->is_directory(dir_ec)) {
                XeniaIOSSweepStalePartFiles(package_it->path());
              }
            }
          }
        }
      }
    }
  }
}

void xe_migrate_nested_dlc_packages() {
  static std::mutex migration_mutex;
  std::lock_guard<std::mutex> lock(migration_mutex);
  const std::filesystem::path user_root =
      xe_get_ios_documents_path() / "content" / "0000000000000000";
  std::error_code ec;
  std::filesystem::directory_iterator it(user_root, ec);
  std::filesystem::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    const std::filesystem::path root = it->path() / "00000002";
    std::vector<std::filesystem::path> folders;
    std::error_code root_ec;
    std::filesystem::directory_iterator package_it(root, root_ec);
    for (; !root_ec && package_it != end; package_it.increment(root_ec)) {
      std::error_code type_ec;
      if (package_it->symlink_status(type_ec).type() == std::filesystem::file_type::directory &&
          !EndsWith(package_it->path().filename().string(), ".data")) {
        folders.push_back(package_it->path());
      }
    }
    for (const auto& folder : folders) {
      if (EndsWith(folder.filename().string(), kMigrateAsideSuffix)) {
        ResumeNestedDlcMigration(root, folder);
      } else {
        MigrateNestedDlcPackage(root, folder);
      }
    }
  }
}

bool xe_link_content_package_into_root(const IOSSelectedContentPackage& package_info,
                                       const std::filesystem::path& destination_root,
                                       const std::filesystem::path& source_relative_path,
                                       std::string* error_message_out) {
  if (error_message_out) {
    *error_message_out = "";
  }

  const std::filesystem::path package_path = destination_root / package_info.path.filename();
  const std::filesystem::path marker_path = LinkedContentMarkerPathForPackage(package_path);
  std::error_code ec;
  std::filesystem::remove(marker_path, ec);

  if (!RebuildLinkedContentProjection(package_path, package_info.path, error_message_out)) {
    std::filesystem::remove_all(package_path, ec);
    std::filesystem::remove_all(ContentSidecarPath(package_path), ec);
    return false;
  }

  if (!WriteLinkedContentMarker(package_path, package_info.path, source_relative_path,
                                error_message_out)) {
    std::filesystem::remove_all(package_path, ec);
    std::filesystem::remove_all(ContentSidecarPath(package_path), ec);
    std::filesystem::remove(marker_path, ec);
    return false;
  }

  XeniaIOSExcludeFromBackup([NSString stringWithUTF8String:destination_root.c_str()]);
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
  std::filesystem::remove(LinkedContentMarkerPathForPackage(entry.path), ec);
}

bool xe_remove_installed_content(const IOSInstalledContentEntry& entry,
                                 std::string* error_message_out) {
  std::error_code ec;
  std::filesystem::remove_all(entry.path, ec);
  // Only a DLC package file has a {name}.data folder of its own.
  if (!ec && entry.kind == IOSInstalledContentKind::kDlc) {
    std::filesystem::remove_all(ContentSidecarPath(entry.path), ec);
  }
  if (ec) {
    if (error_message_out) {
      *error_message_out = ec.message();
    }
    return false;
  }
  xe_remove_linked_content_marker(entry);
  return true;
}

bool xe_remove_title_content(uint32_t title_id, std::string* error_message_out) {
  for (const IOSInstalledContentEntry& entry : xe_list_installed_content(title_id)) {
    if (!xe_remove_installed_content(entry, error_message_out)) {
      return false;
    }
  }
  return true;
}

// {content}/{xuid}/{title_id}/00000001: every profile, and the shared folder a title may use.
static std::vector<std::filesystem::path> TitleSaveRoots(uint32_t title_id) {
  std::vector<std::filesystem::path> roots;
  char title[9] = {};
  std::snprintf(title, sizeof(title), "%08X", title_id);
  char type[9] = {};
  std::snprintf(type, sizeof(type), "%08X", static_cast<uint32_t>(xe::XContentType::kSavedGame));
  std::error_code ec;
  for (const auto& owner :
       std::filesystem::directory_iterator(xe_get_ios_documents_path() / "content", ec)) {
    if (owner.path().filename().string().size() == 16) {
      const std::filesystem::path root = owner.path() / title / type;
      std::error_code exists_ec;
      if (std::filesystem::is_directory(root, exists_ec)) {
        roots.push_back(root);
      }
    }
  }
  return roots;
}

bool xe_title_has_saves(uint32_t title_id) { return !TitleSaveRoots(title_id).empty(); }

bool xe_remove_title_saves(uint32_t title_id, std::string* error_message_out) {
  for (const std::filesystem::path& root : TitleSaveRoots(title_id)) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    if (ec) {
      if (error_message_out) {
        *error_message_out = ec.message();
      }
      return false;
    }
  }
  return true;
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
  for (; !ec && it != end; it.increment(ec)) {
    const std::filesystem::path path = it->path();
    const std::string name = path.filename().string();
    if (EndsWith(name, kLinkedContentMarkerSuffix) || EndsWith(name, ".part")) {
      continue;
    }
    std::error_code type_ec;
    const auto link_status = it->symlink_status(type_ec);
    if (type_ec) {
      continue;
    }
    if (std::filesystem::is_directory(link_status)) {
      // A package file's ".data" sidecar is part of that package.
      if (EndsWith(name, ".data")) {
        std::filesystem::path package_path = path;
        package_path.replace_extension();
        std::error_code package_ec;
        if (std::filesystem::exists(std::filesystem::symlink_status(package_path, package_ec)) &&
            !std::filesystem::is_directory(package_path, package_ec)) {
          continue;
        }
      }
    } else if (kind != IOSInstalledContentKind::kDlc ||
               !(std::filesystem::is_regular_file(link_status) ||
                 std::filesystem::is_symlink(link_status))) {
      // DLC is a package file (or a link to one, which stays listed while its
      // external folder is away); title updates are installed extracted.
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
