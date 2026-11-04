/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_PATCHES_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_GAME_PATCHES_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include <cstdint>

#include "xenia/ui/ios/shared/ios_view_helpers.h"

namespace xe {
namespace ui {
class IOSWindowedAppContext;
struct IOSPatchEntrySummary;
}  // namespace ui
}  // namespace xe

// A patch's name as written for people: trimmed, capitalised, "Patch 3" when it has none.
NSString* XeniaPatchDisplayName(const xe::ui::IOSPatchEntrySummary& patch);

// Per-title patch toggle sheet backed by the same bundled patch database and
// storage override files used by the desktop UI.
@interface XeniaGamePatchesViewController : XESheetTableViewController
- (instancetype)initWithTitleID:(uint32_t)titleID
                          title:(NSString*)title
                     appContext:(xe::ui::IOSWindowedAppContext*)appContext;
@end

#endif  // XENIA_UI_IOS_GAME_PATCHES_VIEW_CONTROLLER_H_
