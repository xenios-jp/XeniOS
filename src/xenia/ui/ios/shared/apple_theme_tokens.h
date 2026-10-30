/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_APPLE_THEME_TOKENS_H_
#define XENIA_UI_APPLE_THEME_TOKENS_H_

#include <cstdint>

namespace xe {
namespace ui {
namespace apple {

// Snapshot source: the xenios-website repo's `src/app/globals.css`.
// Keep this module as the canonical token definition for Apple-native UI.
enum class ThemeVariant : uint8_t {
  kDark = 0,
  kLight = 1,
};

struct ColorRGBA8 {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;

  constexpr float Rf() const { return float(r) / 255.0f; }
  constexpr float Gf() const { return float(g) / 255.0f; }
  constexpr float Bf() const { return float(b) / 255.0f; }
  constexpr float Af() const { return float(a) / 255.0f; }
};

struct ThemeColorTokens {
  ColorRGBA8 bg_primary;
  ColorRGBA8 bg_surface;
  ColorRGBA8 bg_surface_2;
  ColorRGBA8 bg_surface_3;
  ColorRGBA8 text_primary;
  ColorRGBA8 text_secondary;
  ColorRGBA8 text_muted;
  ColorRGBA8 accent;
  ColorRGBA8 accent_hover;
  ColorRGBA8 accent_fg;
  ColorRGBA8 border;
  ColorRGBA8 border_hover;
  ColorRGBA8 status_playable;
  ColorRGBA8 status_ingame;
  ColorRGBA8 status_intro;
  ColorRGBA8 status_loads;
  ColorRGBA8 status_nothing;
  ColorRGBA8 overlay;
  ColorRGBA8 overlay_light;
};

struct ThemeTypographyTokens {
  const char* font_sans;
  const char* font_mono;
};

struct ThemeRadiusTokens {
  float xs;
  float md;
  float lg;
  float xl;
  float xxl;
};

// Shadow recipe. shadowColor is always black; the trait-aware alpha lives in
// opacity_dark / opacity_light so a layer can pick the right value at the
// trait collection it was instantiated under.
struct ThemeShadow {
  float radius;
  float opacity_dark;
  float opacity_light;
  float offset_x;
  float offset_y;
};

struct ThemeShadowTokens {
  ThemeShadow subtle;    // floating window chrome
  ThemeShadow medium;    // status toast / focus glow
  ThemeShadow elevated;  // achievement notification / sheet stack
};

struct ThemeSpacingTokens {
  float xs;
  float sm;
  float md;
  float lg;
  float xl;
  float xxl;
};

struct ThemeOpacityTokens {
  float subtle;
  float soft;
  float medium;
  float strong;
  float heavy;
};

// Each entry pairs a point size with a UIFontWeight value (which is a CGFloat
// typedef on Apple platforms: regular=0.0, medium=0.23, semibold=0.3,
// bold=0.4). The Objective-C side maps each style to a UIFontTextStyle for
// Dynamic Type scaling.
struct ThemeTypographyStyle {
  float point_size;
  float weight;
};

struct ThemeTypographyScale {
  ThemeTypographyStyle caption2;     // 10pt medium
  ThemeTypographyStyle caption1;     // 12pt regular
  ThemeTypographyStyle footnote;     // 13pt regular
  ThemeTypographyStyle subheadline;  // 15pt regular
  ThemeTypographyStyle body;         // 17pt regular
  ThemeTypographyStyle headline;     // 17pt semibold
  ThemeTypographyStyle title3;       // 20pt semibold
  ThemeTypographyStyle title2;       // 22pt semibold
};

// Branded categorical icon palette — Xenia equivalents for the system colors
// previously used by the settings hub
// (systemBlue/Indigo/Orange/Purple/Red/Gray). Variant-aware so each picks the
// correct shade for dark and light traits.
struct ThemeSectionIconTokens {
  ColorRGBA8 blue;
  ColorRGBA8 indigo;
  ColorRGBA8 orange;
  ColorRGBA8 purple;
  ColorRGBA8 red;
  ColorRGBA8 gray;
};

// On-screen touch overlay tint palette. The same seven colours that
// IOSTouchTintStyle enumerates. Not theme-variant-aware because the overlay
// renders above game content rather than above iOS chrome.
struct ThemeTouchTintTokens {
  ColorRGBA8 amber;
  ColorRGBA8 sky;
  ColorRGBA8 mint;
  ColorRGBA8 rose;
  ColorRGBA8 lime;
  ColorRGBA8 coral;
  ColorRGBA8 slate;
};

struct ThemeTokens {
  ThemeColorTokens colors;
  ThemeTypographyTokens typography;
  ThemeRadiusTokens radius;
  ThemeShadowTokens shadows;
  ThemeSpacingTokens spacing;
  ThemeOpacityTokens opacity;
  ThemeTypographyScale typography_scale;
  ThemeSectionIconTokens section_icons;
};

const ThemeTokens& GetThemeTokens(ThemeVariant variant);
const ThemeTokens& GetDarkThemeTokens();
const ThemeTokens& GetLightThemeTokens();

// Variant-independent — touch overlay colours don't follow iOS dark/light.
const ThemeTouchTintTokens& GetTouchTintTokens();

}  // namespace apple
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_APPLE_THEME_TOKENS_H_
