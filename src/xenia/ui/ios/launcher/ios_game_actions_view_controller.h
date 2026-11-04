/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_GAME_ACTIONS_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_GAME_ACTIONS_VIEW_CONTROLLER_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#include <cstdint>

#import "xenia/ui/ios/shared/ios_view_helpers.h"

@protocol XeniaGameContentHost;
@class XeniaIOSZarConversionChoice;

namespace xe {
namespace ui {
class IOSWindowedAppContext;
}  // namespace ui
}  // namespace xe

typedef NS_ENUM(NSInteger, XeniaIOSGameAction) {
  XeniaIOSGameActionPlay = 0,
  XeniaIOSGameActionGameSettings,
  XeniaIOSGameActionResetGameSettings,
  XeniaIOSGameActionTouchLayout,
  XeniaIOSGameActionCompatibility,
  XeniaIOSGameActionManageContent,
  XeniaIOSGameActionLaunchDisc,
  XeniaIOSGameActionPatches,
  XeniaIOSGameActionConvertToZar,
  XeniaIOSGameActionCopyLaunchURL,
  XeniaIOSGameActionSystemLink,
};

// The game page: the game's art, its cover and status, Play with touch layout and
// settings beside it, then the rest of what can be done with the game.
@interface XeniaIOSGameActionsViewController : XESheetViewController

@property(nonatomic, copy) void (^actionHandler)(XeniaIOSGameAction action);
// The compatibility status under the title ("● Playable"); hidden when nil.
@property(nonatomic, copy) NSString* statusText;
@property(nonatomic, retain) UIColor* statusColor;
// Where the Patches row reads and saves its switches when it opens in place.
@property(nonatomic, assign) xe::ui::IOSWindowedAppContext* appContext;
// Just the rows, for the Library to show in place of its grid beside art it already shows. Set
// before the view loads.
@property(nonatomic, assign) BOOL showsRowsOnly;
// Every row opens in place under its heading. What the rows need besides the title ID:
// Launch Disc's discs, by label with where each is under it; |discHandler| plays one.
@property(nonatomic, copy) NSArray<NSString*>* discLabels;
@property(nonatomic, copy) NSArray<NSString*>* discDetails;
@property(nonatomic, copy) void (^discHandler)(NSUInteger discIndex);
// Updates & Add-ons installs through it and asks it to rescan the library.
@property(nonatomic, assign) id<XeniaGameContentHost> contentHost;
// The capsule beside Play: the compatibility status and performance codes, and the notes.
@property(nonatomic, copy) NSString* compatibilityStatus;
@property(nonatomic, copy) NSString* compatibilityPerformance;
@property(nonatomic, copy) NSString* compatibilityNotes;
// Convert to ZAR: fetches the ways to convert, and the size, when the row opens.
@property(nonatomic, copy) void (^conversionChoicesLoader)
    (void (^update)(NSString* summary, BOOL estimating,
                    NSArray<XeniaIOSZarConversionChoice*>* choices));
// What ⋯ offers: copy the launch link, reset the game's settings.
- (UIMenu*)moreMenu;

- (instancetype)initWithGameTitle:(NSString*)gameTitle
                          titleID:(uint32_t)titleID
            supportsCompatibility:(BOOL)supportsCompatibility
            supportsManageContent:(BOOL)supportsManageContent
            supportsDiscSelection:(BOOL)supportsDiscSelection
                  supportsPatches:(BOOL)supportsPatches
            supportsZarConversion:(BOOL)supportsZarConversion;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_GAME_ACTIONS_VIEW_CONTROLLER_H_
