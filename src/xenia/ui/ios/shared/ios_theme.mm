/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_theme.h"

#include "xenia/ui/ios/shared/apple_theme_tokens.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {

// Wraps a ColorRGBA8 token (from apple_theme_tokens.cc) as a UIColor.
inline UIColor* XEColorFromToken(const xe::ui::apple::ColorRGBA8& c) {
  return [UIColor colorWithRed:c.Rf() green:c.Gf() blue:c.Bf() alpha:c.Af()];
}

// Returns the per-trait token RGBA8 for the given accessor.
//   accessor: a lambda that pulls a ColorRGBA8 field from ThemeColorTokens.
inline UIColor* XEDynamicTokenColor(
    xe::ui::apple::ColorRGBA8 (^accessor)(const xe::ui::apple::ThemeColorTokens&)) {
  return [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
    const auto& tokens = (traits.userInterfaceStyle == UIUserInterfaceStyleLight)
                             ? xe::ui::apple::GetLightThemeTokens()
                             : xe::ui::apple::GetDarkThemeTokens();
    return XEColorFromToken(accessor(tokens.colors));
  }];
}

// Same pattern but pulls from the section-icon palette, which is part of the
// per-variant ThemeTokens (so dark/light pick different shades automatically).
inline UIColor* XEDynamicSectionIconColor(
    xe::ui::apple::ColorRGBA8 (^accessor)(const xe::ui::apple::ThemeSectionIconTokens&)) {
  return [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
    const auto& tokens = (traits.userInterfaceStyle == UIUserInterfaceStyleLight)
                             ? xe::ui::apple::GetLightThemeTokens()
                             : xe::ui::apple::GetDarkThemeTokens();
    return XEColorFromToken(accessor(tokens.section_icons));
  }];
}

// Picks the right shadow recipe for the given elevation. Same recipe in both
// variants; only the alpha shifts.
inline const xe::ui::apple::ThemeShadow& XEShadowForElevation(XeniaShadowElevation elevation) {
  const auto& shadows = xe::ui::apple::GetDarkThemeTokens().shadows;
  switch (elevation) {
    case XeniaShadowElevationSubtle:
      return shadows.subtle;
    case XeniaShadowElevationMedium:
      return shadows.medium;
    case XeniaShadowElevationElevated:
      return shadows.elevated;
  }
  return shadows.subtle;
}

}  // namespace

XeniaShadowGeometry XeniaShadowGeometryForElevation(XeniaShadowElevation elevation) {
  const auto& shadow = XEShadowForElevation(elevation);
  return (XeniaShadowGeometry){
      .radius = shadow.radius,
      .offset = CGSizeMake(shadow.offset_x, shadow.offset_y),
  };
}

XeniaTextStyle XeniaTextStyleCaption2(void) {
  const auto& s = xe::ui::apple::GetDarkThemeTokens().typography_scale.caption2;
  return (XeniaTextStyle){s.point_size, s.weight, UIFontTextStyleCaption2};
}
XeniaTextStyle XeniaTextStyleCaption1(void) {
  const auto& s = xe::ui::apple::GetDarkThemeTokens().typography_scale.caption1;
  return (XeniaTextStyle){s.point_size, s.weight, UIFontTextStyleCaption1};
}
XeniaTextStyle XeniaTextStyleFootnote(void) {
  const auto& s = xe::ui::apple::GetDarkThemeTokens().typography_scale.footnote;
  return (XeniaTextStyle){s.point_size, s.weight, UIFontTextStyleFootnote};
}
XeniaTextStyle XeniaTextStyleSubheadline(void) {
  const auto& s = xe::ui::apple::GetDarkThemeTokens().typography_scale.subheadline;
  return (XeniaTextStyle){s.point_size, s.weight, UIFontTextStyleSubheadline};
}
XeniaTextStyle XeniaTextStyleBody(void) {
  const auto& s = xe::ui::apple::GetDarkThemeTokens().typography_scale.body;
  return (XeniaTextStyle){s.point_size, s.weight, UIFontTextStyleBody};
}
XeniaTextStyle XeniaTextStyleHeadline(void) {
  const auto& s = xe::ui::apple::GetDarkThemeTokens().typography_scale.headline;
  return (XeniaTextStyle){s.point_size, s.weight, UIFontTextStyleHeadline};
}
XeniaTextStyle XeniaTextStyleTitle3(void) {
  const auto& s = xe::ui::apple::GetDarkThemeTokens().typography_scale.title3;
  return (XeniaTextStyle){s.point_size, s.weight, UIFontTextStyleTitle3};
}
XeniaTextStyle XeniaTextStyleTitle2(void) {
  const auto& s = xe::ui::apple::GetDarkThemeTokens().typography_scale.title2;
  return (XeniaTextStyle){s.point_size, s.weight, UIFontTextStyleTitle2};
}

@implementation XeniaTheme
// No static caching — this file is compiled under MRC (manual reference
// counting), so static locals would hold dangling pointers after the
// autorelease pool drains. UIColor creation is cheap; callers retain.
//
// Each accessor returns a +colorWithDynamicProvider: UIColor that resolves the
// concrete RGBA on access via the current trait collection's
// userInterfaceStyle. Dark/Light values come from
// apple_theme_tokens.cc (kThemeDark / kThemeLight). statusError /
// statusWarning aren't in the token table; their dark values match the
// previous hardcoded UI, and their light values are darker so they keep
// sufficient contrast on a white background.
+ (UIColor*)bgPrimary {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.bg_primary;
  });
}
+ (UIColor*)bgSurface {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.bg_surface;
  });
}
+ (UIColor*)bgSurface2 {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.bg_surface_2;
  });
}
+ (UIColor*)bgSurface3 {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.bg_surface_3;
  });
}
+ (UIColor*)textPrimary {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.text_primary;
  });
}
+ (UIColor*)textSecondary {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.text_secondary;
  });
}
+ (UIColor*)textMuted {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.text_muted;
  });
}
+ (UIColor*)accent {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.accent;
  });
}
+ (UIColor*)accentHover {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.accent_hover;
  });
}
+ (UIColor*)accentFg {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.accent_fg;
  });
}
+ (UIColor*)statusError {
  return [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
    return (traits.userInterfaceStyle == UIUserInterfaceStyleLight) ? XEColorFromHexRGB(0xdc2626)
                                                                    : XEColorFromHexRGB(0xf87171);
  }];
}
+ (UIColor*)statusWarning {
  return [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
    return (traits.userInterfaceStyle == UIUserInterfaceStyleLight) ? XEColorFromHexRGB(0xd97706)
                                                                    : XEColorFromHexRGB(0xfbbf24);
  }];
}
+ (UIColor*)border {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.border;
  });
}
+ (UIColor*)borderHover {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.border_hover;
  });
}
+ (UIColor*)overlay {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.overlay;
  });
}
+ (UIColor*)overlayLight {
  return XEDynamicTokenColor(^(const xe::ui::apple::ThemeColorTokens& c) {
    return c.overlay_light;
  });
}
+ (UIColor*)sectionIconBlue {
  return XEDynamicSectionIconColor(^(const xe::ui::apple::ThemeSectionIconTokens& c) {
    return c.blue;
  });
}
+ (UIColor*)sectionIconIndigo {
  return XEDynamicSectionIconColor(^(const xe::ui::apple::ThemeSectionIconTokens& c) {
    return c.indigo;
  });
}
+ (UIColor*)sectionIconOrange {
  return XEDynamicSectionIconColor(^(const xe::ui::apple::ThemeSectionIconTokens& c) {
    return c.orange;
  });
}
+ (UIColor*)sectionIconPurple {
  return XEDynamicSectionIconColor(^(const xe::ui::apple::ThemeSectionIconTokens& c) {
    return c.purple;
  });
}
+ (UIColor*)sectionIconRed {
  return XEDynamicSectionIconColor(^(const xe::ui::apple::ThemeSectionIconTokens& c) {
    return c.red;
  });
}
+ (UIColor*)sectionIconGray {
  return XEDynamicSectionIconColor(^(const xe::ui::apple::ThemeSectionIconTokens& c) {
    return c.gray;
  });
}
+ (UIColor*)touchTintAmber {
  return XEColorFromToken(xe::ui::apple::GetTouchTintTokens().amber);
}
+ (UIColor*)touchTintSky {
  return XEColorFromToken(xe::ui::apple::GetTouchTintTokens().sky);
}
+ (UIColor*)touchTintMint {
  return XEColorFromToken(xe::ui::apple::GetTouchTintTokens().mint);
}
+ (UIColor*)touchTintRose {
  return XEColorFromToken(xe::ui::apple::GetTouchTintTokens().rose);
}
+ (UIColor*)touchTintLime {
  return XEColorFromToken(xe::ui::apple::GetTouchTintTokens().lime);
}
+ (UIColor*)touchTintCoral {
  return XEColorFromToken(xe::ui::apple::GetTouchTintTokens().coral);
}
+ (UIColor*)touchTintSlate {
  return XEColorFromToken(xe::ui::apple::GetTouchTintTokens().slate);
}
+ (UIColor*)shadowColorForElevation:(XeniaShadowElevation)elevation {
  const auto& shadow = XEShadowForElevation(elevation);
  const CGFloat alpha_dark = shadow.opacity_dark;
  const CGFloat alpha_light = shadow.opacity_light;
  return [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
    const CGFloat alpha = (traits.userInterfaceStyle == UIUserInterfaceStyleLight)
                              ? alpha_light
                              : alpha_dark;
    return [UIColor colorWithWhite:0.0 alpha:alpha];
  }];
}
+ (CGFloat)spacingXs { return xe::ui::apple::GetDarkThemeTokens().spacing.xs; }
+ (CGFloat)spacingSm { return xe::ui::apple::GetDarkThemeTokens().spacing.sm; }
+ (CGFloat)spacingMd { return xe::ui::apple::GetDarkThemeTokens().spacing.md; }
+ (CGFloat)spacingLg { return xe::ui::apple::GetDarkThemeTokens().spacing.lg; }
+ (CGFloat)spacingXl { return xe::ui::apple::GetDarkThemeTokens().spacing.xl; }
+ (CGFloat)spacingXxl { return xe::ui::apple::GetDarkThemeTokens().spacing.xxl; }
+ (CGFloat)opacitySubtle { return xe::ui::apple::GetDarkThemeTokens().opacity.subtle; }
+ (CGFloat)opacitySoft { return xe::ui::apple::GetDarkThemeTokens().opacity.soft; }
+ (CGFloat)opacityMedium { return xe::ui::apple::GetDarkThemeTokens().opacity.medium; }
+ (CGFloat)opacityStrong { return xe::ui::apple::GetDarkThemeTokens().opacity.strong; }
+ (CGFloat)opacityHeavy { return xe::ui::apple::GetDarkThemeTokens().opacity.heavy; }
@end
