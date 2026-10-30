/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_hero_glow_palette.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#import "xenia/ui/ios/shared/ios_theme.h"

BOOL xe_color_to_rgb_components(UIColor* color, CGFloat* r, CGFloat* g, CGFloat* b, CGFloat* a) {
  if (!color || !r || !g || !b || !a) {
    return NO;
  }
  if ([color getRed:r green:g blue:b alpha:a]) {
    return YES;
  }
  CGFloat white = 0.0;
  if ([color getWhite:&white alpha:a]) {
    *r = white;
    *g = white;
    *b = white;
    return YES;
  }
  return NO;
}

UIColor* xe_blend_rgb_colors(UIColor* a, UIColor* b, CGFloat amount) {
  if (!a) {
    return b ?: [XeniaTheme accent];
  }
  if (!b) {
    return a;
  }
  CGFloat ra = 0.0, ga = 0.0, ba = 0.0, aa = 1.0;
  CGFloat rb = 0.0, gb = 0.0, bb = 0.0, ab = 1.0;
  if (!xe_color_to_rgb_components(a, &ra, &ga, &ba, &aa) ||
      !xe_color_to_rgb_components(b, &rb, &gb, &bb, &ab)) {
    return a;
  }
  CGFloat t = MIN(1.0, MAX(0.0, amount));
  return [UIColor colorWithRed:ra + (rb - ra) * t
                         green:ga + (gb - ga) * t
                          blue:ba + (bb - ba) * t
                         alpha:aa + (ab - aa) * t];
}

CGFloat xe_color_luma(UIColor* color) {
  CGFloat r = 0.0, g = 0.0, b = 0.0, a = 1.0;
  if (!xe_color_to_rgb_components(color, &r, &g, &b, &a)) {
    return 0.0;
  }
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

namespace {

CGFloat xe_wrap_unit(CGFloat value) {
  CGFloat wrapped = fmod(value, 1.0);
  if (wrapped < 0.0) {
    wrapped += 1.0;
  }
  return wrapped;
}

CGFloat xe_shortest_hue_delta(CGFloat from, CGFloat to) {
  CGFloat delta = xe_wrap_unit(to) - xe_wrap_unit(from);
  if (delta > 0.5) {
    delta -= 1.0;
  }
  if (delta < -0.5) {
    delta += 1.0;
  }
  return delta;
}

double xe_hue_from_rgb(double r, double g, double b) {
  double max_v = std::max({r, g, b});
  double min_v = std::min({r, g, b});
  double delta = max_v - min_v;
  if (delta <= 1e-6) {
    return 0.0;
  }
  double h = 0.0;
  if (max_v == r) {
    h = (g - b) / delta;
  } else if (max_v == g) {
    h = 2.0 + (b - r) / delta;
  } else {
    h = 4.0 + (r - g) / delta;
  }
  h /= 6.0;
  if (h < 0.0) {
    h += 1.0;
  }
  return h;
}

UIColor* xe_color_from_weighted_rgb(double r, double g, double b, double weight) {
  if (weight <= 0.0) {
    return nil;
  }
  return [UIColor colorWithRed:(CGFloat)(r / weight)
                         green:(CGFloat)(g / weight)
                          blue:(CGFloat)(b / weight)
                         alpha:1.0];
}

UIColor* xe_adjusted_glow_color(UIColor* color, BOOL secondary) {
  if (!color) {
    return [XeniaTheme accent];
  }
  CGFloat h = 0.0, s = 0.0, v = 0.0, a = 1.0;
  if (![color getHue:&h saturation:&s brightness:&v alpha:&a]) {
    CGFloat r = 0.0, g = 0.0, b = 0.0;
    if (![color getRed:&r green:&g blue:&b alpha:&a]) {
      return [XeniaTheme accent];
    }
    color = [UIColor colorWithRed:r green:g blue:b alpha:1.0];
    if (![color getHue:&h saturation:&s brightness:&v alpha:&a]) {
      return [XeniaTheme accent];
    }
  }

  UIColor* mint = [XeniaTheme accent];
  CGFloat mh = 0.0, ms = 0.0, mv = 0.0, ma = 1.0;
  [mint getHue:&mh saturation:&ms brightness:&mv alpha:&ma];
  (void)ms;
  (void)mv;
  (void)ma;

  if (s < 0.11) {
    CGFloat hue = xe_wrap_unit(mh + xe_shortest_hue_delta(mh, h) * 0.22);
    CGFloat sat = secondary ? 0.30 : 0.36;
    CGFloat val = MIN(1.0, MAX(secondary ? 0.56 : 0.62, v * (secondary ? 1.20 : 1.28)));
    return [UIColor colorWithHue:hue saturation:sat brightness:val alpha:1.0];
  }

  h = xe_wrap_unit(h);
  s = MIN(0.98, MAX(secondary ? 0.40 : 0.48, s * (secondary ? 1.36 : 1.52)));
  v = MIN(1.0,
          MAX(secondary ? 0.58 : 0.64, v * (secondary ? 1.24 : 1.34) + (secondary ? 0.02 : 0.05)));
  if (secondary) {
    h = xe_wrap_unit(h + xe_shortest_hue_delta(h, mh) * 0.05);
  }
  return [UIColor colorWithHue:h saturation:s brightness:v alpha:1.0];
}

}  // namespace

XEHeroGlowPalette xe_extract_hero_glow_palette(UIImage* image) {
  XEHeroGlowPalette palette;
  palette.primary = [XeniaTheme accent];
  palette.secondary = [XeniaTheme accentHover];
  if (!image || !image.CGImage) {
    return palette;
  }

  static constexpr size_t kSampleW = 40;
  static constexpr size_t kSampleH = 56;
  static constexpr size_t kHueBins = 24;
  static constexpr size_t kBytesPerPixel = 4;
  static constexpr size_t kStride = kSampleW * kBytesPerPixel;
  static constexpr double kTopIgnoreRatio = 0.20;
  uint8_t pixels[kSampleH][kStride];
  memset(pixels, 0, sizeof(pixels));

  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  if (!color_space) {
    return palette;
  }
  CGContextRef ctx = CGBitmapContextCreate(
      pixels, kSampleW, kSampleH, 8, kStride, color_space,
      kCGBitmapByteOrder32Big | static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
  CGColorSpaceRelease(color_space);
  if (!ctx) {
    return palette;
  }
  CGContextSetInterpolationQuality(ctx, kCGInterpolationLow);
  CGContextDrawImage(ctx, CGRectMake(0, 0, kSampleW, kSampleH), image.CGImage);
  CGContextRelease(ctx);

  double hue_weight[kHueBins] = {};
  double hue_r[kHueBins] = {};
  double hue_g[kHueBins] = {};
  double hue_b[kHueBins] = {};
  double primary_hue_weight[kHueBins] = {};
  double primary_hue_r[kHueBins] = {};
  double primary_hue_g[kHueBins] = {};
  double primary_hue_b[kHueBins] = {};
  double accent_hue_weight[kHueBins] = {};
  double accent_hue_r[kHueBins] = {};
  double accent_hue_g[kHueBins] = {};
  double accent_hue_b[kHueBins] = {};
  double neutral_r = 0.0, neutral_g = 0.0, neutral_b = 0.0, neutral_w = 0.0;
  double total_r = 0.0, total_g = 0.0, total_b = 0.0, total_w = 0.0;

  const size_t top_ignore_row = (size_t)floor((double)kSampleH * kTopIgnoreRatio);
  const double usable_rows = (double)(kSampleH - top_ignore_row);
  for (size_t y = top_ignore_row; y < kSampleH; ++y) {
    double row_t = usable_rows > 0.0 ? ((double)(y - top_ignore_row) / usable_rows) : 0.0;
    for (size_t x = 0; x < kSampleW; ++x) {
      size_t idx = x * kBytesPerPixel;
      double r = pixels[y][idx] / 255.0;
      double g = pixels[y][idx + 1] / 255.0;
      double b = pixels[y][idx + 2] / 255.0;
      double a = pixels[y][idx + 3] / 255.0;
      if (a < 0.10) {
        continue;
      }

      double max_v = std::max({r, g, b});
      double min_v = std::min({r, g, b});
      double delta = max_v - min_v;
      double sat = max_v > 1e-6 ? (delta / max_v) : 0.0;
      double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
      if (luma < 0.02) {
        continue;
      }

      double center_x = ((double)x + 0.5) / (double)kSampleW;
      double center_bias = 0.70 + 0.30 * (1.0 - fabs(center_x - 0.5) * 2.0);
      double vertical_bias = 0.86 + 0.14 * row_t;
      double base_w = a * center_bias * vertical_bias;
      double top_bias = 1.28 - row_t * 0.84;
      double bottom_bias = 0.30 + row_t * 1.18;

      double total_weight = base_w * (0.12 + luma * 0.72 + sat * 0.64);
      total_r += r * total_weight;
      total_g += g * total_weight;
      total_b += b * total_weight;
      total_w += total_weight;

      if (sat >= 0.12) {
        double hue = xe_hue_from_rgb(r, g, b);
        size_t bin = (size_t)floor(hue * (double)kHueBins) % kHueBins;
        double weight = base_w * (0.16 + sat * 2.05 + luma * 0.82);
        hue_weight[bin] += weight;
        hue_r[bin] += r * weight;
        hue_g[bin] += g * weight;
        hue_b[bin] += b * weight;
        double primary_weight = weight * top_bias;
        primary_hue_weight[bin] += primary_weight;
        primary_hue_r[bin] += r * primary_weight;
        primary_hue_g[bin] += g * primary_weight;
        primary_hue_b[bin] += b * primary_weight;
        double accent_weight = weight * bottom_bias;
        accent_hue_weight[bin] += accent_weight;
        accent_hue_r[bin] += r * accent_weight;
        accent_hue_g[bin] += g * accent_weight;
        accent_hue_b[bin] += b * accent_weight;
      } else {
        double neutral_weight = base_w * (0.14 + luma * 0.86);
        neutral_r += r * neutral_weight;
        neutral_g += g * neutral_weight;
        neutral_b += b * neutral_weight;
        neutral_w += neutral_weight;
      }
    }
  }

  size_t primary_bin = 0;
  for (size_t i = 1; i < kHueBins; ++i) {
    if (primary_hue_weight[i] > primary_hue_weight[primary_bin]) {
      primary_bin = i;
    }
  }
  if (primary_hue_weight[primary_bin] <= 0.0) {
    for (size_t i = 1; i < kHueBins; ++i) {
      if (hue_weight[i] > hue_weight[primary_bin]) {
        primary_bin = i;
      }
    }
  }

  UIColor* primary = nil;
  UIColor* secondary = nil;
  if (primary_hue_weight[primary_bin] > 0.0) {
    primary =
        xe_color_from_weighted_rgb(primary_hue_r[primary_bin], primary_hue_g[primary_bin],
                                   primary_hue_b[primary_bin], primary_hue_weight[primary_bin]);
  } else if (hue_weight[primary_bin] > 0.0) {
    primary = xe_color_from_weighted_rgb(hue_r[primary_bin], hue_g[primary_bin], hue_b[primary_bin],
                                         hue_weight[primary_bin]);
  }
  if (!primary) {
    primary = xe_color_from_weighted_rgb(total_r, total_g, total_b, total_w);
  }

  if (accent_hue_weight[primary_bin] > 0.0 || hue_weight[primary_bin] > 0.0) {
    size_t secondary_bin = kHueBins;
    for (size_t i = 0; i < kHueBins; ++i) {
      if (i == primary_bin) {
        continue;
      }
      size_t distance = (i > primary_bin) ? (i - primary_bin) : (primary_bin - i);
      distance = std::min(distance, kHueBins - distance);
      if (distance < 2) {
        continue;
      }
      if (accent_hue_weight[i] < accent_hue_weight[primary_bin] * 0.24 &&
          hue_weight[i] < hue_weight[primary_bin] * 0.24) {
        continue;
      }
      double i_weight = accent_hue_weight[i] > 0.0 ? accent_hue_weight[i] : hue_weight[i];
      double best_weight = (secondary_bin == kHueBins) ? -1.0
                                                       : (accent_hue_weight[secondary_bin] > 0.0
                                                              ? accent_hue_weight[secondary_bin]
                                                              : hue_weight[secondary_bin]);
      if (i_weight > best_weight) {
        secondary_bin = i;
      }
    }
    if (secondary_bin != kHueBins) {
      if (accent_hue_weight[secondary_bin] > 0.0) {
        secondary = xe_color_from_weighted_rgb(
            accent_hue_r[secondary_bin], accent_hue_g[secondary_bin], accent_hue_b[secondary_bin],
            accent_hue_weight[secondary_bin]);
      } else {
        secondary = xe_color_from_weighted_rgb(hue_r[secondary_bin], hue_g[secondary_bin],
                                               hue_b[secondary_bin], hue_weight[secondary_bin]);
      }
    }
  }

  if (!secondary && neutral_w > 0.0) {
    secondary = xe_color_from_weighted_rgb(neutral_r, neutral_g, neutral_b, neutral_w);
  }
  if (!secondary) {
    secondary = primary ? xe_blend_rgb_colors(primary, [XeniaTheme accentHover], 0.28)
                        : [XeniaTheme accentHover];
  }
  if (!primary) {
    primary = [XeniaTheme accent];
  }

  palette.primary = xe_adjusted_glow_color(primary, NO);
  palette.secondary = xe_adjusted_glow_color(secondary, YES);
  return palette;
}
