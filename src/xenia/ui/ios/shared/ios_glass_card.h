/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GLASS_CARD_H_
#define XENIA_UI_IOS_GLASS_CARD_H_

#import <UIKit/UIKit.h>

// The glass card (docs/ios_design_language.md, rule 2): short things opened from a control,
// such as Profiles and Public Games, float over the dimmed screen instead of covering it.

// The card's widest: enough for a list on iPad; on a phone the card spans the screen less its
// insets. Centred in the safe area when narrower than it.
constexpr CGFloat kXEGlassCardMaxWidth = 440.0;

// Makes |navigation| present as the card: inset from the screen edges under the status bar,
// radius 34, Liquid Glass, as tall as the page it shows (up to the screen), with the root page's
// title on the leading side of its bar. A tap beside the card closes it. Call before presenting
// it (and before touching its presentationController).
void XEPrepareGlassCard(UINavigationController* navigation);

// Gives |card| the card's surface: iOS 26 Liquid Glass (regular, lightly darkened so text holds
// over bright art), radius 34 with continuous corners, clipping its content. Shared by the
// presented card and the Friends card.
void XEInstallGlassCardSurface(UIView* card);

// YES when |page| (or the navigation controller holding it) is shown as the card. Such pages
// skip the art wash: the card's own glass is their backdrop.
BOOL XEIsInGlassCard(UIViewController* page);

// Fits the card to |page|'s table: call from viewWillAppear and viewDidLayoutSubviews. Does
// nothing outside a card, or for a page that isn't on top.
void XEFitGlassCardToTable(UITableViewController* page);

// Fits the card to |page|'s content, |height| points under the bar: for pages that aren't a
// table. Call from viewDidLayoutSubviews. Does nothing outside a card.
void XEFitGlassCardToHeight(UIViewController* page, CGFloat height);

#endif  // XENIA_UI_IOS_GLASS_CARD_H_
