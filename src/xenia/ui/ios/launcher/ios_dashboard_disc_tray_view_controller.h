/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_DASHBOARD_DISC_TRAY_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_DASHBOARD_DISC_TRAY_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include "xenia/ui/ios/shared/ios_view_helpers.h"

// A disc in the library, as the tray lists it.
@interface XeniaDashboardDisc : NSObject
@property(nonatomic, copy) NSString* title;
// Which disc, its format and the linked folder it is in, like
// "Disc 1 of 2 · ZAR · Backup".
@property(nonatomic, copy) NSString* detail;
@property(nonatomic, copy) NSString* path;
// The game's icon, or nil.
@property(nonatomic, copy) NSString* iconPath;
@end

// The dashboard's Open Tray: the library's discs, to put one in the drive or
// leave it empty. Answers exactly once, with the disc's path or nil, however
// it goes away.
@interface XeniaDashboardDiscTrayViewController : XESheetTableViewController
- (instancetype)initWithDiscs:(NSArray<XeniaDashboardDisc*>*)discs
                   completion:(void (^)(NSString* path))completion;
// Answers nil, as leaving the drive empty does, if nothing answered yet.
- (void)answerEmpty;
@end

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_DASHBOARD_DISC_TRAY_VIEW_CONTROLLER_H_
