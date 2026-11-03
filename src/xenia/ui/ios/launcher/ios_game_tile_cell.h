/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_TILE_CELL_H_
#define XENIA_UI_IOS_GAME_TILE_CELL_H_

#import <UIKit/UIKit.h>

#include <cstdint>

#include "xenia/ui/ios/shared/ios_theme.h"

// Collection-view cell for the launcher's library: card with cover artwork,
// title and optional compatibility pill. Border, shadow and focus ring follow
// the cell's configuration state (focus, highlight, controller focus) and
// resolve their colors against the state's trait collection.
@interface XeniaGameTileCell : UICollectionViewCell
@property(nonatomic, strong) UIView* cardView;
@property(nonatomic, strong) UIImageView* iconView;
@property(nonatomic, strong) UILabel* titleLabel;
@property(nonatomic, strong) XeniaPaddedLabel* contentTypePill;
@property(nonatomic, strong) XeniaPaddedLabel* compatPill;
@property(nonatomic, assign) BOOL controllerFocused;
// Title id of the game the cell currently shows (0 when none). Lets async
// artwork loads check that the cell has not been reused.
@property(nonatomic, assign) uint32_t representedTitleId;
@end

#endif  // XENIA_UI_IOS_GAME_TILE_CELL_H_
