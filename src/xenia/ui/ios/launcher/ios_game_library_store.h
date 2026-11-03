/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_LIBRARY_STORE_H_
#define XENIA_UI_IOS_GAME_LIBRARY_STORE_H_

#import <Foundation/Foundation.h>

#include <filesystem>
#include <vector>

#include "xenia/ui/ios/launcher/ios_file_transfer.h"
#include "xenia/ui/ios/launcher/ios_game_library.h"

#ifdef __OBJC__

@interface XeniaIOSExternalLibraryAccess : NSObject
@property(nonatomic, readonly) NSURL* url;
@property(nonatomic, readonly, getter=isAccessGranted) BOOL accessGranted;
- (instancetype)initWithURL:(NSURL*)url;
@end

#endif  // __OBJC__

namespace xe {
namespace ui {

std::filesystem::path IOSImportedGamesDirectory();
// Symlink-resolved absolute form of a path that may not exist yet; for
// comparing paths.
std::filesystem::path NormalizedIOSLibraryPath(const std::filesystem::path& path);

// Synchronous convenience form. Don't call it on the main thread for real
// files: use the transfer form from a background queue.
std::filesystem::path ImportGameIntoIOSLibrary(NSURL* source_url, NSError** error);
// Blocking; call from a background queue with the source's security scope
// open for the whole call. Copies through "<name>.part" with progress and
// cancel (see ios_file_transfer.h); a source already inside Documents is
// renamed. Returns an empty path with `error` set on failure or cancel.
std::filesystem::path ImportGameIntoIOSLibrary(NSURL* source_url, XeniaIOSFileTransfer* transfer,
                                               XeniaIOSFileTransferProgress progress,
                                               NSError** error);
bool IsPathInIOSImportedGamesDirectory(const std::filesystem::path& path);
// Documents/Inbox is where iOS puts copies of files opened from other apps.
bool IsPathInIOSDocumentsInbox(const std::filesystem::path& path);
// Deletes everything in Documents/Inbox except `keep_path` (empty for none).
// Call at launch, passing the file of a launch-time open-in if there is one.
// Safe from any thread.
void CleanupIOSDocumentsInbox(const std::filesystem::path& keep_path);

// One linked external library folder, for the management UI.
struct IOSExternalLibraryLocation {
  std::string name;            // Display name (folder name, or stored name).
  std::filesystem::path path;  // Folder path (resolved bookmark, or stored path).
  bool available = false;      // Bookmark resolved without going stale.
};
std::vector<IOSExternalLibraryLocation> ListIOSExternalLibraryLocations();

#ifdef __OBJC__

BOOL SaveIOSExternalLibraryLocation(NSURL* folder_url, NSError** error);
BOOL HideIOSExternalLibraryGameAtPath(const std::filesystem::path& path, NSString** hiddenName,
                                      NSError** error);
BOOL RemoveIOSExternalLibraryLocationForPath(const std::filesystem::path& path,
                                             NSString** removedName, NSError** error);
// Removes only the location whose root matches `path` exactly (for the linked
// folders list, where containment matching would also unlink parent folders).
BOOL RemoveIOSExternalLibraryLocationAtRoot(const std::filesystem::path& path,
                                            NSString** removedName, NSError** error);
XeniaIOSExternalLibraryAccess* StartIOSExternalLibraryAccessForPath(
    const std::filesystem::path& path, BOOL* matchedExternalLocation, NSError** error);
XeniaIOSExternalLibraryAccess* StartIOSExternalLibraryAccessForPath(
    const std::filesystem::path& path, BOOL* matchedExternalLocation,
    std::filesystem::path* relativePath, NSError** error);

#endif  // __OBJC__

// Walks Documents and every linked folder, blocking until done, so keep it off
// the main thread. Safe to call from any background queue (several at once are
// fine): it touches no UIKit, starts and stops its security scopes on the
// calling thread, and shares only NSUserDefaults. Also writes the cache below,
// sweeps stale ".part" files from the games folder and keeps the games and
// installed-content folders excluded from iCloud backup.
std::vector<IOSDiscoveredGame> ScanIOSGameLibrary(NSDictionary* title_name_cache);

// The result of the last completed scan, read from disk (cheap, no folder
// walk). For showing the library at once while the real scan runs; entries
// are display-only until the scan replaces them. Empty on first launch.
std::vector<IOSDiscoveredGame> LoadCachedIOSGameLibrary();

void ApplyIOSCompatibilityDataToDiscoveredGames(NSDictionary* compat_data,
                                                std::vector<IOSDiscoveredGame>* games);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_GAME_LIBRARY_STORE_H_
