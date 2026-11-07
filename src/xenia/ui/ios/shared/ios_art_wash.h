/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_ART_WASH_H_
#define XENIA_UI_IOS_ART_WASH_H_

#import <UIKit/UIKit.h>

// The one backdrop (docs/ios_design_language.md, rule 1): the art of the game in context,
// blurred, under a dim. The library publishes its featured game's art; every sheet shows it.

// A small, heavily blurred copy of |image|: art too small to show large reads as colour and
// light instead of as blurry pixels. nil for nil.
UIImage* XEBlurredBackdropImage(UIImage* image);

// The current wash, already blurred. The library sets it when its featured game changes; open
// washes follow.
void XESetArtWashImage(UIImage* blurred);
UIImage* XEArtWashImage(void);

// The blurred art, aspect-filled, under the page dim. Flat page colour while there is no art.
@interface XEArtWashView : UIView
@end

// Puts |page| on the wash: its own backgrounds go clear, table rows take the glass group look,
// and one wash sits behind the navigation (or split) controller that holds it, so pushed pages
// slide over a backdrop that stays put. Call from viewDidLoad; the XESheet* base classes finish
// the job in viewWillAppear, once the page is in its container.
void XEUseArtWash(UIViewController* page);
void XEInstallArtWashIfNeeded(UIViewController* page);

#endif  // XENIA_UI_IOS_ART_WASH_H_
