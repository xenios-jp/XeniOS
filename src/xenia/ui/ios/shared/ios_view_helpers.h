/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_VIEW_HELPERS_H_
#define XENIA_UI_IOS_VIEW_HELPERS_H_

#import <UIKit/UIKit.h>

#include <cstdint>
#include <string>

#import "xenia/ui/ios/shared/ios_theme.h"

// Shared ObjC-side helpers for the iOS UI module: bridges from std::string
// to NSString, the standard "OK" alert presenter, and sheet base classes
// that own the orientation overrides every Xenia modal sheet shares.

NSString* ToNSString(const std::string& value);

// Title ID → 8-digit hex NSString (zero-padded), uppercase or lowercase.
NSString* XEFormatTitleIDHexUpper(uint32_t title_id);
NSString* XEFormatTitleIDHexLower(uint32_t title_id);

// Builds a fully opaque UIColor from a 0xRRGGBB packed value.
UIColor* XEColorFromHexRGB(uint32_t rgb);

// Applies common VoiceOver metadata. Pass nil for values that are not needed.
void XEApplyAccessibility(UIView* view, NSString* label, NSString* value, NSString* hint,
                          UIAccessibilityTraits traits);

// Configures a UINavigationController as a XeniOS task sheet. On compact
// height devices the sheet attaches to the screen edge and follows
// preferredContentSize so landscape iPhone sheets don't stretch into a
// cut-off fullscreen bar.
void XEConfigureTaskSheet(UINavigationController* navigation_controller, UIView* host_view,
                          CGSize preferred_size, BOOL prevent_interactive_dismissal);

// Configures a controller as a full destination for hierarchical app areas
// such as Settings and Profile. Unlike XEConfigureTaskSheet, this never uses a
// medium detent: compact/iPhone presentations are fullscreen, while iPad
// presentations use a large-only page sheet.
void XEConfigureDestinationControllerPresentation(UIViewController* view_controller,
                                                  UIView* host_view, CGSize preferred_size,
                                                  BOOL prevent_interactive_dismissal);

// Convenience wrapper for navigation-controller-backed destinations.
void XEConfigureDestinationPresentation(UINavigationController* navigation_controller,
                                        UIView* host_view, CGSize preferred_size,
                                        BOOL prevent_interactive_dismissal);

// Returns an autoreleased UIVisualEffect suitable as the backdrop for a
// floating chrome surface (in-game menu, touch overlay editor, status toast,
// achievement banner). Hand the result straight to
// `-[UIVisualEffectView initWithEffect:]` which retains it.
//
// On iOS 26+ this returns a `UIGlassEffect` (regular or clear variant per
// `clear_variant`). On iOS 18-25 the fallback is
// `UIBlurEffectStyleSystemMaterial` regardless of `clear_variant` —
// pre-iOS-26 callers see the same backdrop they got before this helper
// existed.
UIVisualEffect* xe_make_chrome_visual_effect(BOOL clear_variant);

// Configures `view` with the standard floating-window styling used across the
// iOS chrome (touch editor, in-game menu, etc.):
//
//   * 20pt corner radius
//   * 1pt hairline border (10%-white in dark mode, 10%-black in light mode)
//   * Soft 8pt shadow at 14% black, offset (0, 8)
//   * Liquid-glass backdrop on iOS 26+ (regular variant), with
//     UIBlurEffectStyleSystemMaterial as the iOS 18-25 fallback. Picked by
//     xe_make_chrome_visual_effect().
//
// `view` should be a plain UIView with `backgroundColor = clearColor`. The
// helper inserts the blur as the bottommost subview so any subsequent
// addSubview: lands on top.
//
// Internally the view is converted to an XeniaFloatingWindowView subclass
// which observes -traitCollectionDidChange: and refreshes its border-color
// CGColor when userInterfaceStyle flips.
void xe_apply_floating_window_chrome(UIView* view);

// Lighter cousin of xe_apply_floating_window_chrome for less-elevated
// surfaces (status toast, achievement banner, settings restart-notice card):
//
//   * XeniaRadiusLg (12pt) corner radius
//   * 1pt hairline border, same dark/light dynamic as the floating window
//   * No shadow — caller layers one on via xe_apply_shadow_token().
//   * Backdrop picked by xe_make_chrome_visual_effect(clear_variant). For
//     the toast/banner use case pass NO (regular glass on iOS 26).
//
// `view` should be a plain UIView with `backgroundColor = clearColor` and the
// caller is responsible for inserting content subviews above the inserted
// backdrop (which lands at index 0).
void xe_apply_glass_card_chrome(UIView* view, BOOL clear_variant);

// Applies the tokenised shadow recipe for the given elevation to view.layer.
// shadowColor is [XeniaTheme shadowColorForElevation:elevation] so dark/light
// trait flips read the right alpha; shadowOpacity stays 1.0. The CGColor is
// re-resolved on -traitCollectionDidChange: through a hidden trait observer
// attached to `view` (same pattern as xe_apply_floating_window_chrome).
void xe_apply_shadow_token(UIView* view, XeniaShadowElevation elevation);

// Hidden zero-frame UIView that forwards -traitCollectionDidChange: into a
// block. Used internally by xe_apply_floating_window_chrome to keep the
// floating-window border / blur in sync with the trait collection without
// changing the helper's existing UIView* signature.
@interface XeniaTraitObserverView : UIView
@property(nonatomic, copy) void (^onTraitChange)(UITraitCollection* previousTraits);
@end

// Presents a one-button "OK" alert. No-ops when `presenter` is nil.
void XEPresentOKAlert(UIViewController* presenter, NSString* title, NSString* message);

// UITableViewController base class with the orientation overrides every
// Xenia sheet shares: rotates freely (no upside-down) and starts in
// whatever orientation the host launcher is currently using.
@interface XESheetTableViewController : UITableViewController
@end

// UIViewController base class with the same orientation overrides as
// XESheetTableViewController.
@interface XESheetViewController : UIViewController
@end

#endif  // XENIA_UI_IOS_VIEW_HELPERS_H_
