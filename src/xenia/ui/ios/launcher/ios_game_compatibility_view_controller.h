/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_COMPATIBILITY_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_GAME_COMPATIBILITY_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include <cstdint>

#include "xenia/ui/ios/shared/ios_view_helpers.h"

// Per-game compatibility hero sheet. Renders the cover/background hero card
// with status / perf pills, build pill and the most recent app-submitted
// reports + GitHub discussion summary, plus animated radial-glow accents
// keyed off the cover artwork's palette.
@interface XeniaGameCompatibilityViewController : XESheetTableViewController
- (instancetype)initWithTitleID:(uint32_t)title_id
                          title:(NSString*)title
                     compatData:(NSDictionary*)compat_data;
- (void)setHeroArtwork:(UIImage*)image;
@end

#endif  // XENIA_UI_IOS_GAME_COMPATIBILITY_VIEW_CONTROLLER_H_
