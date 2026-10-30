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

// Cover and background artwork cache for game tiles, hero cards and
// compatibility sheets.
//
// Tile (cover) artwork is fetched from Element18592/360-Game-Art keyed by the
// lower-case hex title id and stored in
//   Library/Caches/game-art/{title_id_hex}.jpg.
// Background artwork is fetched from xenia-manager/x360db (upper-case hex)
// and stored in
//   Library/Caches/game-background-art/{title_id_hex_lower}.jpg.

UIImage* _Nullable xe_cached_game_art(uint32_t title_id);
UIImage* _Nullable xe_cached_game_background_art(uint32_t title_id);

void xe_fetch_game_art(uint32_t title_id, void (^completion)(UIImage* _Nullable image));
void xe_fetch_game_background_art(uint32_t title_id, void (^completion)(UIImage* _Nullable image));

NS_ASSUME_NONNULL_END

#endif  // XENIA_UI_IOS_GAME_ART_H_
