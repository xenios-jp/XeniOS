/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_STORAGE_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_STORAGE_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// What the app keeps on the device, by kind and by game, and the clean-ups that
// free it. Measuring walks the container: call it off the main thread.

enum class IOSStorageKind : int {
  kGames,      // Imported discs and packages
  kContent,    // Title updates and add-ons
  kSaves,      // Saves and profiles
  kGameData,   // The emulated cache drives games install to
  kShaders,    // Saved shaders and pipelines, the system's Metal cache
  kOther,      // System software, art, logs, small caches
  kLeftovers,  // Nothing uses them: archiver copies, old temporary files
  kLinked,     // Games in linked folders that are on this device's own storage
  kCount,
};

using IOSStorageBytes = std::array<uint64_t, size_t(IOSStorageKind::kCount)>;

// One disc file of a library game.
struct IOSStorageDisc {
  // As the library spells it, to find the disc again for Delete Disc.
  std::string path;
  std::string label;
  // The library plays it from a linked folder: deleting it deletes the file
  // there.
  bool linked = false;
  // What deleting it frees: the file with its ".data" folder, or a folder
  // game's folder. 0 when it couldn't be sized (a linked folder that can't be
  // reached, a folder game in a linked folder).
  uint64_t bytes = 0;
};

struct IOSStorageTitle {
  // 0 for a library game whose title ID couldn't be read; game_path names it.
  uint32_t title_id = 0;
  std::string name;
  bool in_library = false;
  // A copy of the game is in the app's own storage.
  bool has_imported_copy = false;
  // A copy of the game is in a linked folder (outside the app's storage).
  bool has_linked_copy = false;
  // What the linked copies take where they are, when the folder is reachable.
  // The part on this device's own storage (On My iPhone) is also in
  // bytes[kLinked] and Total(); a copy on another drive is only here.
  uint64_t linked_bytes = 0;
  // The library entry's path, to find the game again for Delete Game.
  std::string game_path;
  // A library game's disc files, in the library's order.
  std::vector<IOSStorageDisc> discs;
  IOSStorageBytes bytes = {};
  uint64_t Total() const;
};

struct IOSStorageReport {
  IOSStorageBytes bytes = {};
  // Largest first.
  std::vector<IOSStorageTitle> titles;
  uint64_t Total() const;
};

// Bytes taken on disk (allocated blocks, not file lengths).
IOSStorageReport xe_ios_measure_storage();

// The shader storage root (cache_root/shaders lives under it).
std::filesystem::path xe_ios_cache_root();

// Removes the archiver's copies and temporary files older than a day. Only
// while no game runs: a game's archive save writes a copy. Returns the bytes
// freed.
uint64_t xe_ios_remove_storage_leftovers();

// Empties the emulated cache drives. Games that installed data there do it
// again. Only while no game runs.
bool xe_ios_clear_game_data_drives(std::string* error_message_out);

// A title's saved shaders and pipelines.
bool xe_ios_clear_title_shaders(uint32_t title_id, uintmax_t* removed_out,
                                std::error_code* error_out);
// With `system_cache`, also the system's compiled Metal cache in the app's
// Caches (rebuilt as games run; only while none does).
bool xe_ios_clear_all_shaders(uintmax_t* removed_out, std::error_code* error_out,
                              bool system_cache = false);

// Cover, background and dashboard art of a title no longer in the library.
void xe_ios_remove_title_art(uint32_t title_id);

// Everything kept for a title that isn't in the library: its updates, add-ons
// and saves under every profile, its shaders and its art.
bool xe_ios_remove_title_data(uint32_t title_id, std::string* error_message_out);

// At launch, before a game can run: leftovers, cache drives kept out of the
// backup, CPU caches unused for 60 days. Runs on a background queue.
void xe_ios_storage_launch_maintenance();

#ifdef __OBJC__
#import <Foundation/Foundation.h>
// Posted on the main queue after the library was scanned again (a game was
// added or deleted), so the Storage screen measures again.
extern NSNotificationName const XeniaStorageDidChangeNotification;
// Posted by the Storage screen to have the launcher offer to delete a library
// game, with its title ID (NSNumber) under XeniaStorageTitleIdKey and its
// library path (NSString) under XeniaStorageGamePathKey.
extern NSNotificationName const XeniaStorageDeleteGameRequestNotification;
extern NSString* const XeniaStorageTitleIdKey;
extern NSString* const XeniaStorageGamePathKey;
// Posted by the Storage screen, once the user confirmed, to have the launcher
// delete one disc file of a library game that has several: the game as above,
// and the disc's library path (NSString) under XeniaStorageDiscPathKey.
extern NSNotificationName const XeniaStorageDeleteDiscRequestNotification;
extern NSString* const XeniaStorageDiscPathKey;
#endif  // __OBJC__

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_STORAGE_H_
