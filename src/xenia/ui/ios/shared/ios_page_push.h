/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_PAGE_PUSH_H_
#define XENIA_UI_IOS_PAGE_PUSH_H_

#import <UIKit/UIKit.h>

// A page pushed inside a page that isn't a navigation stack: the phone's game page, which the
// library draws in place (docs/ios_design_language.md, rule 7). Patches, Updates & Add-ons,
// Game Settings and the rest slide in from the trailing edge over the game's own art, which
// stays still; |covered| (the page's title, buttons and rows) fades back under them.

// Makes |navigation| present that way. Its root gets ‹, which slides it back out to the page;
// an edge swipe does the same. Closing it any other way (its ✕) closes the page under it too,
// through |close_all|. Call before presenting it.
void XEPreparePagePush(UINavigationController* navigation, NSArray<UIView*>* covered,
                       void (^close_all)(void));

// YES when |page| (or the navigation controller holding it) was presented that way. Such
// pages skip the art wash: the page under them is their backdrop.
BOOL XEIsPagePush(UIViewController* page);

#endif  // XENIA_UI_IOS_PAGE_PUSH_H_
