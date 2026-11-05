/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_LIBRARY_AMBIENT_VIEW_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_LIBRARY_AMBIENT_VIEW_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

// What the Library draws behind its art and grid (Settings › Appearance › Library Background).
typedef NS_ENUM(NSInteger, XeniaLibraryBackground) {
  XeniaLibraryBackgroundGameColours = 0,  // The colours of the game the Library shows.
  XeniaLibraryBackgroundGreen = 1,        // XeniOS greens.
  XeniaLibraryBackgroundOff = 2,          // The plain page colour.
};

typedef NS_ENUM(NSInteger, XeniaLibraryBackgroundMotion) {
  XeniaLibraryBackgroundMotionSubtle = 0,
  XeniaLibraryBackgroundMotionStill = 1,
};

XeniaLibraryBackground XeniaLibraryBackgroundSetting(void);
void XeniaSetLibraryBackgroundSetting(XeniaLibraryBackground background);
XeniaLibraryBackgroundMotion XeniaLibraryBackgroundMotionSetting(void);
void XeniaSetLibraryBackgroundMotionSetting(XeniaLibraryBackgroundMotion motion);
// Posted on the main queue when either setting, or the game colours, change.
extern NSNotificationName const XeniaLibraryBackgroundDidChangeNotification;

// The colours a background draws with: the shown game's (the green ones until it has some),
// or the greens.
NSArray<UIColor*>* XeniaLibraryBackgroundColours(XeniaLibraryBackground background);
// A rounded swatch of |colours| for a settings row; a plain grey one for nil.
UIImage* XeniaLibraryBackgroundSwatch(NSArray<UIColor*>* colours, CGFloat side);

// Copies of the shown game's art turning slowly around the page under a heavy blur, toned so
// white text reads over them, from the Library Background setting. It draws only while it can be
// seen: it holds still when Reduce Motion or Low Power Mode is on or the device is hot, and stops
// while the app is in the background, a game is running or something covers the Library.
@interface XeniaLibraryAmbientView : UIView

// The image the Library shows for its game (art or cover); its art and colours are made off the
// main thread, once per |key|, and the field fades to them. nil for no game.
- (void)setGameImage:(UIImage*)image key:(NSString*)key;
// Checks again whether the Library can be seen (the launcher is up, the app is in front and
// nothing covers it), after the launcher was shown or hidden.
- (void)visibilityMayHaveChanged;
// Where the Library's art lies in this view (|frame|) and the part of the image it shows
// (|contents|, as a layer's contentsRect): the field lays a copy of the art there, so the art runs
// on into a blur of itself. CGRectNull when the Library shows no art.
- (void)setAnchorFrame:(CGRect)frame contentsRect:(CGRect)contents;
// Whether it draws anything (the setting isn't Off); the page behind it shows otherwise.
@property(nonatomic, readonly) BOOL drawsBackground;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_LIBRARY_AMBIENT_VIEW_H_
