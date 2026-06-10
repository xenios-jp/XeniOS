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

std::filesystem::path ImportGameIntoIOSLibrary(NSURL* source_url, NSError** error);
bool IsPathInIOSImportedGamesDirectory(const std::filesystem::path& path);

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

#endif  // __OBJC__

std::vector<IOSDiscoveredGame> ScanIOSGameLibrary(NSDictionary* title_name_cache);

void ApplyIOSCompatibilityDataToDiscoveredGames(NSDictionary* compat_data,
                                                std::vector<IOSDiscoveredGame>* games);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_GAME_LIBRARY_STORE_H_
