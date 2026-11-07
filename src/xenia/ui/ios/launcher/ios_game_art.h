/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_ART_H_
#define XENIA_UI_IOS_GAME_ART_H_

#import <UIKit/UIKit.h>

#include <cstdint>

NS_ASSUME_NONNULL_BEGIN

// Posted on the main thread when a title's cover lands in the cache, whoever asked for it. Its
// object is the title id (NSNumber). A tile that gave up on its cover picks it up from this.
extern NSNotificationName const XeniaGameArtCoverDidArriveNotification;

// Cover and background artwork cache for game tiles, hero cards and
// compatibility sheets.
//
// Tile (cover) artwork comes from edge's box art sources for the title
// (xe::app::GetBoxArtUrls: x360db, then the marketplace, or xdb for an
// original Xbox game), tried in order, and is stored in
//   Library/Caches/game-boxart/{title_id_hex}.jpg.
// Background artwork is fetched from xenia-manager/x360db (upper-case hex)
// and stored in
//   Library/Caches/game-background-art/{title_id_hex_lower}.jpg.

UIImage* _Nullable xe_cached_game_art(uint32_t title_id);
UIImage* _Nullable xe_cached_game_background_art(uint32_t title_id);
// The cached cover's file, nil when there isn't one.
NSString* _Nullable xe_cached_game_art_path(uint32_t title_id);
// Whether the background art is on disk, without reading it.
BOOL xe_has_cached_game_background_art(uint32_t title_id);

// In-memory cache of pre-decoded, downsampled covers keyed by title id.
// `xe_cached_game_art_thumbnail` is a memory-only lookup and is safe on the
// main thread. `xe_load_game_art_thumbnail` reads and decodes the cached cover
// file on a background queue (longest side `max_pixels`), stores it in the
// memory cache, and calls `completion` on the main thread with the image, or
// nil when there is no cover file. The cache is purged on memory warnings.
UIImage* _Nullable xe_cached_game_art_thumbnail(uint32_t title_id);
void xe_load_game_art_thumbnail(uint32_t title_id, CGFloat max_pixels,
                                void (^completion)(UIImage* _Nullable image));
void xe_purge_game_art_cache(void);

// Deletes cached covers for titles not in `keep_title_ids` (NSNumber uint32)
// that have not been shown for 30 days. Runs off the main thread.
void xe_prune_game_art_cache(NSSet<NSNumber*>* keep_title_ids);

// Completions run on the main thread, exactly once; nil on failure or when
// online features are off. Duplicate requests share one download.
void xe_fetch_game_art(uint32_t title_id, void (^completion)(UIImage* _Nullable image));
void xe_fetch_game_background_art(uint32_t title_id, void (^completion)(UIImage* _Nullable image));

NS_ASSUME_NONNULL_END

#endif  // XENIA_UI_IOS_GAME_ART_H_
