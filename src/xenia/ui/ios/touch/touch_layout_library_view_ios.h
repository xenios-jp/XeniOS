/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_LAYOUT_LIBRARY_VIEW_IOS_H_
#define XENIA_UI_IOS_TOUCH_LAYOUT_LIBRARY_VIEW_IOS_H_

#import <UIKit/UIKit.h>

@interface XeniaTouchLayoutLibraryItem : NSObject

@property(nonatomic, copy) NSString* localID;
@property(nonatomic, copy) NSString* displayName;
@property(nonatomic, copy) NSString* author;
@property(nonatomic, assign) BOOL official;
// Pre-rendered miniature preview of this layout's controls. Optional; the
// library cell falls back to a neutral icon when nil.
@property(nonatomic, strong) UIImage* thumbnail;
// True if this layout is the currently bound default for the active game title.
@property(nonatomic, assign) BOOL isDefaultForCurrentTitle;
// True if this layout is used when a game has no title-specific assignment.
@property(nonatomic, assign) BOOL isDefaultForAllGames;
@property(nonatomic, assign) BOOL isFavorite;

@end

@interface XeniaTouchLayoutLibraryRowCell : UITableViewCell

- (UILabel*)overlayTitleLabel;
- (UILabel*)overlaySubtitleLabel;
- (void)setShowsDisclosure:(BOOL)showsDisclosure;
- (void)setShowsCheckmark:(BOOL)showsCheckmark;
- (void)setThumbnailImage:(UIImage*)thumbnail;
- (void)setShowsDefaultBadge:(BOOL)showsDefaultBadge;

@end

#endif  // XENIA_UI_IOS_TOUCH_LAYOUT_LIBRARY_VIEW_IOS_H_
