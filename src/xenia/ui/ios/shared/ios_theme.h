/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_THEME_H_
#define XENIA_UI_IOS_THEME_H_

#import <UIKit/UIKit.h>

// Centralized iOS theme primitives. These mirror the Xenia website tokens
// (see assets/website / apple_theme_tokens) so the UIKit and web surfaces
// stay visually aligned. Keep additions here so view controllers don't grow
// their own private color/font palettes.

// Border radii matching the website's Tailwind scale. XeniaRadiusXs and
// XeniaRadiusXxl are Xenia-iOS additions for small chips and large floating
// surfaces (achievement banner).
static constexpr CGFloat XeniaRadiusXs = 4.0;
static constexpr CGFloat XeniaRadiusMd = 8.0;
static constexpr CGFloat XeniaRadiusLg = 12.0;
static constexpr CGFloat XeniaRadiusXl = 16.0;
static constexpr CGFloat XeniaRadiusXxl = 24.0;

// Shadow elevation steps. Used with [XeniaTheme shadowColorForElevation:] and
// xe_apply_shadow_token() (see ios_view_helpers.h).
typedef NS_ENUM(NSInteger, XeniaShadowElevation) {
  XeniaShadowElevationSubtle = 0,    // floating window chrome
  XeniaShadowElevationMedium = 1,    // status toast / focus glow
  XeniaShadowElevationElevated = 2,  // achievement notification / sheet stack
};

// Geometry for the chosen elevation, in points. Trait-independent.
typedef struct {
  CGFloat radius;
  CGSize offset;
} XeniaShadowGeometry;

XeniaShadowGeometry XeniaShadowGeometryForElevation(XeniaShadowElevation elevation);

// One concrete typography preset — a point size + UIFontWeight + the
// UIFontTextStyle to scale against for Dynamic Type. Picked semantically:
// callers ask for `XeniaTextStyleHeadline()` instead of remembering 17/0.3/Body.
typedef struct {
  CGFloat point_size;
  UIFontWeight weight;
  UIFontTextStyle text_style;
} XeniaTextStyle;

XeniaTextStyle XeniaTextStyleCaption2(void);
XeniaTextStyle XeniaTextStyleCaption1(void);
XeniaTextStyle XeniaTextStyleFootnote(void);
XeniaTextStyle XeniaTextStyleSubheadline(void);
XeniaTextStyle XeniaTextStyleBody(void);
XeniaTextStyle XeniaTextStyleHeadline(void);
XeniaTextStyle XeniaTextStyleTitle3(void);
XeniaTextStyle XeniaTextStyleTitle2(void);

@interface XeniaTheme : NSObject

// Surfaces.
+ (UIColor*)bgPrimary;   // #09090b
+ (UIColor*)bgSurface;   // #18181b
+ (UIColor*)bgSurface2;  // #27272a
+ (UIColor*)bgSurface3;  // #3f3f46

// Text.
+ (UIColor*)textPrimary;    // #fafafa
+ (UIColor*)textSecondary;  // #a1a1aa
+ (UIColor*)textMuted;      // #71717a

// Accent.
+ (UIColor*)accent;       // #34d399
+ (UIColor*)accentHover;  // #6ee7b7
+ (UIColor*)accentFg;     // #09090b

// Status.
+ (UIColor*)statusError;    // #f87171
+ (UIColor*)statusWarning;  // #fbbf24

// Strokes / overlays.
+ (UIColor*)border;        // white 6%
+ (UIColor*)borderHover;   // white 10%
+ (UIColor*)overlay;       // black 85%
+ (UIColor*)overlayLight;  // black 58%

// Branded categorical icon palette for the settings hub and similar surfaces.
// Replaces the previous UIColor.system{Blue,Indigo,Orange,Purple,Red,Gray}Color
// calls so the icon row stays on-brand rather than OS-default.
+ (UIColor*)sectionIconBlue;
+ (UIColor*)sectionIconIndigo;
+ (UIColor*)sectionIconOrange;
+ (UIColor*)sectionIconPurple;
+ (UIColor*)sectionIconRed;
+ (UIColor*)sectionIconGray;

// On-screen touch overlay tints — single source of truth for the seven colours
// that IOSTouchTintStyle enumerates. These render over the game view rather
// than over iOS chrome, so they aren't trait-aware.
+ (UIColor*)touchTintAmber;
+ (UIColor*)touchTintSky;
+ (UIColor*)touchTintMint;
+ (UIColor*)touchTintRose;
+ (UIColor*)touchTintLime;
+ (UIColor*)touchTintCoral;
+ (UIColor*)touchTintSlate;

// Dynamic shadow color for the given elevation. Always black; the alpha
// resolves at the trait collection it's evaluated in (dark mode reads darker
// than light mode). Pair with shadowOpacity = 1.0 on the layer.
+ (UIColor*)shadowColorForElevation:(XeniaShadowElevation)elevation;

// Spacing scale — replaces the 6/8/10/12/14/16/18/24/32 magic numbers seen
// throughout the iOS UI. Use the closest semantic step rather than picking
// an arbitrary number.
+ (CGFloat)spacingXs;
+ (CGFloat)spacingSm;
+ (CGFloat)spacingMd;
+ (CGFloat)spacingLg;
+ (CGFloat)spacingXl;
+ (CGFloat)spacingXxl;

// Recurring opacity values for overlays, tints and chips.
+ (CGFloat)opacitySubtle;
+ (CGFloat)opacitySoft;
+ (CGFloat)opacityMedium;
+ (CGFloat)opacityStrong;
+ (CGFloat)opacityHeavy;

@end

#import "xenia/ui/ios/shared/ios_hero_glow_palette.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"

#endif  // XENIA_UI_IOS_THEME_H_
