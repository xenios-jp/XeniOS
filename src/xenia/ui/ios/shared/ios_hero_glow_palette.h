/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_HERO_GLOW_PALETTE_H_
#define XENIA_UI_IOS_HERO_GLOW_PALETTE_H_

#import <UIKit/UIKit.h>

// Hero artwork -> glow color extraction (used by the compatibility hero card).
typedef struct {
  UIColor* primary;
  UIColor* secondary;
} XEHeroGlowPalette;

// Color math shared by hero/detail UI modules.
BOOL xe_color_to_rgb_components(UIColor* color, CGFloat* r, CGFloat* g, CGFloat* b, CGFloat* a);
UIColor* xe_blend_rgb_colors(UIColor* a, UIColor* b, CGFloat amount);
CGFloat xe_color_luma(UIColor* color);

XEHeroGlowPalette xe_extract_hero_glow_palette(UIImage* image);

#endif  // XENIA_UI_IOS_HERO_GLOW_PALETTE_H_
