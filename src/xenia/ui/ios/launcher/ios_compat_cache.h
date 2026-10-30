/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_COMPAT_CACHE_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_COMPAT_CACHE_H_

#import "xenia/ui/ios/launcher/ios_compat_data.h"

NSString* xe_compat_cache_path(void);
NSDictionary* xe_parse_compat_json(NSData* data);
NSArray* xe_parse_compat_games_array(NSData* data);
NSArray* xe_merge_remote_compat_games_with_cached_submissions(NSArray* remote_games,
                                                              NSArray* cached_games);

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_COMPAT_CACHE_H_
