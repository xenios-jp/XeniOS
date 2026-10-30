/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/shared/apple_theme_tokens.h"

namespace xe {
namespace ui {
namespace apple {

namespace {

// Non-color tokens are theme-independent — same values in dark and light. They
// live inside ThemeTokens for uniform access (`tokens.spacing.lg` etc.) and
// are repeated below in each variant.
constexpr ThemeShadowTokens kSharedShadows = {
    /* subtle   */ {/*radius*/ 8.0f, /*dark*/ 0.14f, /*light*/ 0.10f, 0.0f,
                    8.0f},
    /* medium   */
    {/*radius*/ 14.0f, /*dark*/ 0.42f, /*light*/ 0.18f, 0.0f, 8.0f},
    /* elevated */
    {/*radius*/ 18.0f, /*dark*/ 0.55f, /*light*/ 0.22f, 0.0f, 10.0f},
};

constexpr ThemeSpacingTokens kSharedSpacing = {
    /* xs  */ 4.0f,
    /* sm  */ 8.0f,
    /* md  */ 12.0f,
    /* lg  */ 16.0f,
    /* xl  */ 24.0f,
    /* xxl */ 32.0f,
};

constexpr ThemeOpacityTokens kSharedOpacity = {
    /* subtle */ 0.06f,
    /* soft   */ 0.10f,
    /* medium */ 0.16f,
    /* strong */ 0.28f,
    /* heavy  */ 0.42f,
};

// UIFontWeight values: regular=0.0, medium=0.23, semibold=0.3.
constexpr ThemeTypographyScale kSharedTypographyScale = {
    /* caption2    */ {10.0f, 0.23f},
    /* caption1    */ {12.0f, 0.0f},
    /* footnote    */ {13.0f, 0.0f},
    /* subheadline */ {15.0f, 0.0f},
    /* body        */ {17.0f, 0.0f},
    /* headline    */ {17.0f, 0.3f},
    /* title3      */ {20.0f, 0.3f},
    /* title2      */ {22.0f, 0.3f},
};

constexpr ThemeRadiusTokens kSharedRadius = {
    /* xs  */ 4.0f,
    /* md  */ 8.0f,
    /* lg  */ 12.0f,
    /* xl  */ 16.0f,
    /* xxl */ 24.0f,
};

constexpr ThemeTokens kThemeDark = {
    ThemeColorTokens{
        /* bg_primary */ {0x09, 0x09, 0x0B, 0xFF},
        /* bg_surface */ {0x18, 0x18, 0x1B, 0xFF},
        /* bg_surface_2 */ {0x27, 0x27, 0x2A, 0xFF},
        /* bg_surface_3 */ {0x3F, 0x3F, 0x46, 0xFF},
        /* text_primary */ {0xFA, 0xFA, 0xFA, 0xFF},
        /* text_secondary */ {0xA1, 0xA1, 0xAA, 0xFF},
        /* text_muted */ {0x71, 0x71, 0x7A, 0xFF},
        /* accent */ {0x34, 0xD3, 0x99, 0xFF},
        /* accent_hover */ {0x6E, 0xE7, 0xB7, 0xFF},
        /* accent_fg */ {0x09, 0x09, 0x0B, 0xFF},
        // Website uses oklch(1 0 0 / 0.06) and oklch(1 0 0 / 0.1).
        /* border */ {0xFF, 0xFF, 0xFF, 15},
        /* border_hover */ {0xFF, 0xFF, 0xFF, 26},
        /* status_playable */ {0x34, 0xD3, 0x99, 0xFF},
        /* status_ingame */ {0x60, 0xA5, 0xFA, 0xFF},
        /* status_intro */ {0xFB, 0xBF, 0x24, 0xFF},
        /* status_loads */ {0xFB, 0x92, 0x3C, 0xFF},
        /* status_nothing */ {0xF8, 0x71, 0x71, 0xFF},
        /* overlay */ {0x00, 0x00, 0x00, 217},
        /* overlay_light */ {0x00, 0x00, 0x00, 148},
    },
    ThemeTypographyTokens{
        /* font_sans */ "Inter",
        /* font_mono */ "JetBrains Mono",
    },
    kSharedRadius,
    kSharedShadows,
    kSharedSpacing,
    kSharedOpacity,
    kSharedTypographyScale,
    ThemeSectionIconTokens{
        // Branded equivalents for the system color palette previously used in
        // the settings hub. Picked to read as the same categories while
        // sitting next to the mint accent without clashing.
        /* blue   */ {0x4F, 0x8C, 0xFF, 0xFF},
        /* indigo */ {0x81, 0x8C, 0xF8, 0xFF},
        /* orange */ {0xF5, 0x9E, 0x0B, 0xFF},
        /* purple */ {0xA8, 0x55, 0xF7, 0xFF},
        /* red    */ {0xF8, 0x71, 0x71, 0xFF},
        /* gray   */ {0x71, 0x71, 0x7A, 0xFF},
    },
};

constexpr ThemeTokens kThemeLight = {
    ThemeColorTokens{
        /* bg_primary */ {0xFF, 0xFF, 0xFF, 0xFF},
        /* bg_surface */ {0xF4, 0xF4, 0xF5, 0xFF},
        /* bg_surface_2 */ {0xE4, 0xE4, 0xE7, 0xFF},
        /* bg_surface_3 */ {0xD4, 0xD4, 0xD8, 0xFF},
        /* text_primary */ {0x09, 0x09, 0x0B, 0xFF},
        /* text_secondary */ {0x52, 0x52, 0x5B, 0xFF},
        /* text_muted */ {0xA1, 0xA1, 0xAA, 0xFF},
        /* accent */ {0x10, 0xB9, 0x81, 0xFF},
        /* accent_hover */ {0x05, 0x96, 0x69, 0xFF},
        /* accent_fg */ {0xFF, 0xFF, 0xFF, 0xFF},
        // Website uses oklch(0 0 0 / 0.08) and oklch(0 0 0 / 0.12).
        /* border */ {0x00, 0x00, 0x00, 20},
        /* border_hover */ {0x00, 0x00, 0x00, 31},
        /* status_playable */ {0x34, 0xD3, 0x99, 0xFF},
        /* status_ingame */ {0x60, 0xA5, 0xFA, 0xFF},
        /* status_intro */ {0xFB, 0xBF, 0x24, 0xFF},
        /* status_loads */ {0xFB, 0x92, 0x3C, 0xFF},
        /* status_nothing */ {0xF8, 0x71, 0x71, 0xFF},
        /* overlay */ {0x00, 0x00, 0x00, 217},
        /* overlay_light */ {0x00, 0x00, 0x00, 148},
    },
    ThemeTypographyTokens{
        /* font_sans */ "Inter",
        /* font_mono */ "JetBrains Mono",
    },
    kSharedRadius,
    kSharedShadows,
    kSharedSpacing,
    kSharedOpacity,
    kSharedTypographyScale,
    ThemeSectionIconTokens{
        // Darker variants for the same categorical palette on light
        // backgrounds.
        /* blue   */ {0x25, 0x63, 0xEB, 0xFF},
        /* indigo */ {0x4F, 0x46, 0xE5, 0xFF},
        /* orange */ {0xD9, 0x77, 0x06, 0xFF},
        /* purple */ {0x7C, 0x3A, 0xED, 0xFF},
        /* red    */ {0xDC, 0x26, 0x26, 0xFF},
        /* gray   */ {0x71, 0x71, 0x7A, 0xFF},
    },
};

// Touch overlay tints — single set, matching the historical RGB values that
// were previously duplicated across touch_overlay_style_ios.mm:22-41 and
// touch_layout_library_ios.mm:243-261. These render over game content and
// do not follow the iOS dark/light trait collection.
constexpr ThemeTouchTintTokens kTouchTints = {
    /* amber */ {0xF5, 0xBA, 0x4F, 0xFF},  // 0.96, 0.73, 0.31
    /* sky   */ {0x57, 0xB8, 0xFA, 0xFF},  // 0.34, 0.72, 0.98
    /* mint  */ {0x61, 0xE8, 0xB8, 0xFF},  // 0.38, 0.91, 0.72
    /* rose  */ {0xFA, 0x7A, 0x99, 0xFF},  // 0.98, 0.48, 0.60
    /* lime  */ {0xA8, 0xE3, 0x3D, 0xFF},  // 0.66, 0.89, 0.24
    /* coral */ {0xFA, 0x8F, 0x66, 0xFF},  // 0.98, 0.56, 0.40
    /* slate */ {0x99, 0xAB, 0xC4, 0xFF},  // 0.60, 0.67, 0.77
};

}  // namespace

const ThemeTokens& GetThemeTokens(ThemeVariant variant) {
  return variant == ThemeVariant::kLight ? kThemeLight : kThemeDark;
}

const ThemeTokens& GetDarkThemeTokens() { return kThemeDark; }

const ThemeTokens& GetLightThemeTokens() { return kThemeLight; }

const ThemeTouchTintTokens& GetTouchTintTokens() { return kTouchTints; }

}  // namespace apple
}  // namespace ui
}  // namespace xe
