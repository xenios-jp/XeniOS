/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_library_ambient_view.h"

#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <simd/simd.h>

#include <algorithm>
#include <cmath>
#include <vector>

NSNotificationName const XeniaLibraryBackgroundDidChangeNotification =
    @"XeniaLibraryBackgroundDidChangeNotification";

// A game's art as the field draws it: a small square copy on the GPU, and its average colour for
// the page under the copies.
@interface XeniaAmbientArt : NSObject {
 @public
  id<MTLTexture> texture;
  simd_float3 base;
  float aspect;      // The image's width over its height; the texture is it squeezed square.
  float saturation;  // How much richer the field draws it: more for dull art.
}
@end

@implementation XeniaAmbientArt
- (void)dealloc {
  [texture release];
  [super dealloc];
}
@end

namespace {

NSString* const kBackgroundDefaultsKey = @"XeniaLibraryBackground";
NSString* const kMotionDefaultsKey = @"XeniaLibraryBackgroundMotion";

// The field's clock runs at this fraction of real time at Subtle: the copies circle the page in
// about a minute and turn once in two or three.
constexpr double kSubtleSpeed = 0.3;
// How long the field takes to fade to a new game's art.
constexpr double kArtFade = 1.5;
// The colours are a little more saturated than the art, like a lit backdrop; art with little
// colour (grey skies, brown ground) is pushed further, up to kMaxSaturation, so its hues show.
constexpr float kSaturation = 1.25f;
constexpr float kMaxSaturation = 1.8f;
// The art's average colourfulness (max − min channel) at and above which kSaturation is enough.
constexpr float kColourfulArt = 0.25f;
// The art's copy on the GPU, in pixels a side.
constexpr int kArtSide = 64;
// The copies are drawn at a sixteenth of a point and blurred there, then toned at a quarter of a
// point; the layer scales that up smoothly. Both are far below the screen's resolution, so a
// frame costs very little.
constexpr CGFloat kSceneScale = 1.0 / 16.0;
constexpr CGFloat kOutputScale = 1.0 / 4.0;

// Copies of the art, each circling its own point of the page (|centre|, -1…1 across and down;
// |orbit| the circle's size; |rate| radians per unit of the field's clock), turning at |spin| and
// |size| half-heights of the page across. |phase| sets where each starts. Later ones lie over
// earlier ones; the blur melts their edges together.
struct Piece {
  simd_float2 centre;
  float orbit;
  float rate;
  float spin;
  float size;
  float phase;
  float pad;
};
constexpr Piece kPieces[] = {
    {{-0.35f, 0.55f}, 0.28f, 0.31f, 0.14f, 1.25f, 0.0f, 0.0f},
    {{0.45f, 0.05f}, 0.32f, -0.23f, -0.11f, 1.05f, 2.1f, 0.0f},
    {{-0.15f, -0.60f}, 0.24f, 0.18f, 0.09f, 1.40f, 4.0f, 0.0f},
    {{0.30f, -0.20f}, 0.30f, -0.27f, 0.17f, 0.85f, 5.2f, 0.0f},
};
constexpr int kPieceCount = sizeof(kPieces) / sizeof(kPieces[0]);

// What the shaders read each frame. Matches Field in kShaders.
struct Field {
  float time;
  float fade;
  float aspect;
  float saturation;
  float lo;
  float hi;
  float from_aspect;
  float to_aspect;
  // The copy that lines up with the art the Library shows: where it lies (clip space, top left
  // and size) and the part of the image it shows (texture space).
  simd_float4 anchor_rect;
  simd_float4 anchor_uv;
};
// How far the lined-up copy carries on past the art's edges, as a fraction of its size, fading.
constexpr float kAnchorReach = 0.45f;

NSString* const kShaders = @R"(
#include <metal_stdlib>
using namespace metal;

struct Field {
  float time, fade, aspect, saturation, lo, hi, from_aspect, to_aspect;
  float4 anchor_rect, anchor_uv;
};
struct AnchorOut { float4 position [[position]]; float2 uv; float2 t; };

// The middle square of an image squeezed into a square texture.
float2 square(float2 uv, float aspect) {
  return aspect > 1.0 ? float2(0.5 + (uv.x - 0.5) / aspect, uv.y)
                      : float2(uv.x, 0.5 + (uv.y - 0.5) * aspect);
}
struct Piece { float2 centre; float orbit, rate, spin, size, phase, pad; };
struct PieceOut { float4 position [[position]]; float2 uv; };
struct ScreenOut { float4 position [[position]]; float2 uv; };

// One copy of the art: a square circling its point, turning, and breathing a few percent.
vertex PieceOut piece_vertex(uint v [[vertex_id]], uint i [[instance_id]],
                             constant Field& f [[buffer(0)]],
                             constant Piece* pieces [[buffer(1)]]) {
  const Piece p = pieces[i];
  const float2 corner = float2(float(v & 1u), float(v >> 1)) * 2.0 - 1.0;
  const float a = p.phase + p.rate * f.time;
  const float2 centre =
      (p.centre + p.orbit * float2(cos(a), 0.6 * sin(a))) * float2(f.aspect, -1.0);
  const float turn = p.phase + p.spin * f.time;
  const float size =
      p.size * max(1.0, f.aspect) * (1.0 + 0.05 * sin(0.7 * f.time + 1.7 * p.phase));
  const float2 q = float2x2(cos(turn), sin(turn), -sin(turn), cos(turn)) * (corner * size);
  PieceOut out;
  out.position = float4((q.x + centre.x) / f.aspect, q.y + centre.y, 0.0, 1.0);
  out.uv = float2(corner.x, -corner.y) * 0.5 + 0.5;
  return out;
}

// The art, fading from the last game's to this one's.
fragment half4 piece_fragment(PieceOut in [[stage_in]], constant Field& f [[buffer(0)]],
                              texture2d<half> from [[texture(0)]],
                              texture2d<half> to [[texture(1)]]) {
  constexpr sampler s(filter::linear, address::mirrored_repeat);
  return half4(mix(from.sample(s, square(in.uv, f.from_aspect)).rgb,
                   to.sample(s, square(in.uv, f.to_aspect)).rgb, half(f.fade)),
               1.0h);
}

// The copy under the Library's art, the same size and in the same place, so the art runs on into
// a blur of itself; it carries on a little past the art (the rest of the image, then mirrored)
// and fades out there into the copies that move.
vertex AnchorOut anchor_vertex(uint v [[vertex_id]], constant Field& f [[buffer(0)]],
                               constant float& reach [[buffer(1)]]) {
  const float2 t = float2(float(v & 1u), float(v >> 1)) * (1.0 + 2.0 * reach) - reach;
  AnchorOut out;
  out.position = float4(f.anchor_rect.xy + t * f.anchor_rect.zw, 0.0, 1.0);
  out.uv = f.anchor_uv.xy + t * f.anchor_uv.zw;
  out.t = t;
  return out;
}

fragment half4 anchor_fragment(AnchorOut in [[stage_in]], constant Field& f [[buffer(0)]],
                               constant float& reach [[buffer(1)]],
                               texture2d<half> from [[texture(0)]],
                               texture2d<half> to [[texture(1)]]) {
  constexpr sampler s(filter::linear, address::mirrored_repeat);
  const float2 outside = max(max(-in.t, in.t - 1.0), 0.0);
  const float alpha = 1.0 - smoothstep(0.0, reach, max(outside.x, outside.y));
  return half4(mix(from.sample(s, in.uv).rgb, to.sample(s, in.uv).rgb, half(f.fade)),
               half(alpha));
}

vertex ScreenOut screen_vertex(uint v [[vertex_id]]) {
  const float2 p = float2(float((v << 1) & 2u), float(v & 2u));
  ScreenOut out;
  out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
  out.uv = float2(p.x, 1.0 - p.y);
  return out;
}

// Half of a wide gaussian blur, along |step|.
fragment half4 blur_fragment(ScreenOut in [[stage_in]], constant float2& step [[buffer(0)]],
                             texture2d<half> source [[texture(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  const float w[5] = {1.0, 0.8825, 0.6065, 0.3247, 0.1353};
  float3 sum = float3(source.sample(s, in.uv).rgb) * w[0];
  float total = w[0];
  for (int k = 1; k < 5; ++k) {
    const float2 d = step * float(k);
    sum += float3(source.sample(s, in.uv + d).rgb + source.sample(s, in.uv - d).rgb) * w[k];
    total += 2.0 * w[k];
  }
  return half4(half3(sum / total), 1.0h);
}

// sRGB and OKLab, a space where lightness and colour are apart, so a colour can be darkened
// without going muddy (yellow turns gold, not olive).
float3 linear_from_srgb(float3 c) {
  return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
}
float3 srgb_from_linear(float3 c) {
  c = max(c, 0.0);
  return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308);
}
float3 oklab_from_linear(float3 c) {
  const float3 lms = float3(dot(c, float3(0.4122214708, 0.5363325363, 0.0514459929)),
                            dot(c, float3(0.2119034982, 0.6806995451, 0.1073969566)),
                            dot(c, float3(0.0883024619, 0.2817188376, 0.6299787005)));
  const float3 r = pow(max(lms, 0.0), 1.0 / 3.0);
  return float3(dot(r, float3(0.2104542553, 0.7936177850, -0.0040720468)),
                dot(r, float3(1.9779984951, -2.4285922050, 0.4505937099)),
                dot(r, float3(0.0259040371, 0.7827717662, -0.8086757660)));
}
float3 linear_from_oklab(float3 lab) {
  const float3 r = float3(dot(lab, float3(1.0, 0.3963377774, 0.2158037573)),
                          dot(lab, float3(1.0, -0.1055613458, -0.0638541728)),
                          dot(lab, float3(1.0, -0.0894841775, -1.2914855480)));
  const float3 lms = r * r * r;
  return float3(dot(lms, float3(4.0767416621, -3.3077115913, 0.2309699292)),
                dot(lms, float3(-1.2684380046, 2.6097574011, -0.3413193965)),
                dot(lms, float3(-0.0041960863, -0.7034186147, 1.7076147010)));
}

// Richer colour, then every lightness pressed into a band between |lo| and |hi| with its colour
// kept, so white text reads over any art. A faint fixed dither keeps the dark gradients from
// banding once they're scaled up.
fragment half4 finish_fragment(ScreenOut in [[stage_in]], constant Field& f [[buffer(0)]],
                               texture2d<half> source [[texture(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float3 lab = oklab_from_linear(linear_from_srgb(float3(source.sample(s, in.uv).rgb)));
  lab.yz *= f.saturation;
  const float l = mix(f.lo, f.hi, smoothstep(0.0, 1.0, lab.x));
  // Darker colours hold less chroma before they leave the screen's range: ease it off as the
  // lightness drops, rather than letting it clip.
  const float kept = saturate(l / max(lab.x, 1e-3));
  lab.yz *= mix(0.75, 1.0, kept);
  // A yellow can't be dark and still look yellow; it reads as olive. Turn darkened yellows toward
  // amber instead, as far as they were darkened.
  const float hue = atan2(lab.z, lab.y);
  const float yellow = 1.0 - smoothstep(0.0, 0.6, abs(hue - 1.75));
  const float turn = -0.45 * yellow * (1.0 - kept);
  lab.yz = float2x2(cos(turn), sin(turn), -sin(turn), cos(turn)) * lab.yz;
  lab.x = l;
  float3 c = saturate(srgb_from_linear(linear_from_oklab(lab)));
  const float n = fract(sin(dot(in.position.xy, float2(12.9898, 78.233))) * 43758.5453);
  return half4(half3(c + (n - 0.5) / 255.0), 1.0h);
}
)";

UIColor* RGB(int r, int g, int b) {
  return [UIColor colorWithRed:r / 255.0 green:g / 255.0 blue:b / 255.0 alpha:1.0];
}

NSArray<UIColor*>* GreenColours() {
  static NSArray<UIColor*>* colours = [@[
    RGB(0x14, 0xb8, 0x4c), RGB(0x8a, 0xf0, 0xa8), RGB(0x0a, 0x6b, 0x33), RGB(0x2f, 0xd0, 0x67)
  ] retain];
  return colours;
}

id<MTLDevice> Device() {
  static id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  return device;
}

// The game the Library shows, once its colours and art are ready.
NSArray<UIColor*>* g_game_colours = nil;
XeniaAmbientArt* g_game_art = nil;

NSCache<NSString*, NSArray*>* GameCache() {
  static NSCache* cache = [[NSCache alloc] init];
  return cache;
}

void PostChange() {
  [[NSNotificationCenter defaultCenter]
      postNotificationName:XeniaLibraryBackgroundDidChangeNotification
                    object:nil];
}

// The strongest colours of |image|, at most four: a coarse histogram of its hues on a small copy,
// skipping greys and near-black and near-white, then the four heaviest buckets' averages. Greyish
// art falls back to its faint tints, and plain grey art to slate. For the settings swatch.
NSArray<UIColor*>* ExtractColours(CGImageRef image) {
  constexpr int kSide = 32;
  std::vector<uint8_t> pixels(kSide * kSide * 4);
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(
      pixels.data(), kSide, kSide, 8, kSide * 4, space,
      uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapByteOrder32Big));
  CGColorSpaceRelease(space);
  if (!context) {
    return nil;
  }
  CGContextSetInterpolationQuality(context, kCGInterpolationMedium);
  CGContextDrawImage(context, CGRectMake(0, 0, kSide, kSide), image);
  CGContextRelease(context);

  // 12 hues by 3 brightnesses.
  constexpr int kHues = 12, kValues = 3, kBuckets = kHues * kValues;
  for (const double min_chroma : {0.08, 0.03}) {
    double weight[kBuckets] = {}, sum[kBuckets][3] = {}, total = 0.0;
    for (int i = 0; i < kSide * kSide; ++i) {
      const uint8_t* p = &pixels[i * 4];
      if (p[3] < 128) {
        continue;
      }
      const double r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0;
      const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), chroma = mx - mn;
      if (mx < 0.12 || (mn > 0.9) || chroma < min_chroma) {
        continue;
      }
      double hue;
      if (mx == r) {
        hue = std::fmod((g - b) / chroma + 6.0, 6.0);
      } else if (mx == g) {
        hue = (b - r) / chroma + 2.0;
      } else {
        hue = (r - g) / chroma + 4.0;
      }
      const int h = std::min(kHues - 1, int(hue / 6.0 * kHues));
      const int v = std::min(kValues - 1, int(mx * kValues));
      const int k = h * kValues + v;
      // Mostly by area, so the art's large muted tones count, with vivid pixels a little more.
      const double w = 1.0 + 1.5 * chroma;
      weight[k] += w;
      sum[k][0] += r * w;
      sum[k][1] += g * w;
      sum[k][2] += b * w;
      total += w;
    }
    if (total < 6.0) {
      continue;
    }
    int order[kBuckets];
    for (int k = 0; k < kBuckets; ++k) {
      order[k] = k;
    }
    std::sort(order, order + kBuckets, [&](int a, int b) { return weight[a] > weight[b]; });
    // The heaviest buckets, one per hue first so the swatch isn't four shades of one colour,
    // then any heavy enough.
    std::vector<int> chosen;
    for (int pass = 0; pass < 2 && chosen.size() < 4; ++pass) {
      for (int i = 0; i < kBuckets && chosen.size() < 4; ++i) {
        const int k = order[i];
        if (weight[k] < total * 0.02) {
          break;
        }
        bool skip = false;
        for (int c : chosen) {
          skip |= c == k || (pass == 0 && c / kValues == k / kValues);
        }
        if (!skip) {
          chosen.push_back(k);
        }
      }
    }
    NSMutableArray<UIColor*>* colours = [NSMutableArray array];
    for (int k : chosen) {
      [colours addObject:[UIColor colorWithRed:sum[k][0] / weight[k]
                                         green:sum[k][1] / weight[k]
                                          blue:sum[k][2] / weight[k]
                                         alpha:1.0]];
    }
    // Fewer than four: lighter and darker takes on the strongest.
    UIColor* first = colours.firstObject;
    CGFloat h = 0, s = 0, v = 0, a = 1;
    [first getHue:&h saturation:&s brightness:&v alpha:&a];
    const CGFloat shifts[] = {0.55, 1.35, 0.8};
    for (int i = 0; colours.count < 4; ++i) {
      [colours addObject:[UIColor colorWithHue:h
                                    saturation:s
                                    brightness:std::clamp(v * shifts[i % 3], 0.1, 1.0)
                                         alpha:1.0]];
    }
    // The swatch puts the first in its middle and the third and fourth at its ends.
    return @[ colours[3], colours[1], colours[0], colours[2] ];
  }
  return @[
    RGB(0x3c, 0x4a, 0x58), RGB(0x8a, 0x98, 0xa6), RGB(0x24, 0x2c, 0x34), RGB(0x5c, 0x6a, 0x78)
  ];
}

// |image| squeezed into a small square texture (the field un-squeezes it), with its average
// colour.
XeniaAmbientArt* MakeArt(CGImageRef image) {
  id<MTLDevice> device = Device();
  if (!image || !device) {
    return nil;
  }
  std::vector<uint8_t> pixels(kArtSide * kArtSide * 4);
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(
      pixels.data(), kArtSide, kArtSide, 8, kArtSide * 4, space,
      uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapByteOrder32Big));
  CGColorSpaceRelease(space);
  if (!context) {
    return nil;
  }
  const CGFloat w = CGImageGetWidth(image), h = CGImageGetHeight(image);
  CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
  CGContextDrawImage(context, CGRectMake(0, 0, kArtSide, kArtSide), image);
  CGContextRelease(context);

  double sum[3] = {}, chroma = 0.0;
  for (int i = 0; i < kArtSide * kArtSide; ++i) {
    const uint8_t* p = &pixels[i * 4];
    for (int c = 0; c < 3; ++c) {
      sum[c] += p[c];
    }
    chroma += std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]});
  }
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:kArtSide
                                                        height:kArtSide
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  if (!texture) {
    return nil;
  }
  [texture replaceRegion:MTLRegionMake2D(0, 0, kArtSide, kArtSide)
             mipmapLevel:0
               withBytes:pixels.data()
             bytesPerRow:kArtSide * 4];
  XeniaAmbientArt* art = [[[XeniaAmbientArt alloc] init] autorelease];
  art->texture = texture;
  const double n = 255.0 * kArtSide * kArtSide;
  art->base = simd_make_float3(sum[0] / n, sum[1] / n, sum[2] / n);
  art->aspect = float(std::max<CGFloat>(w, 1.0) / std::max<CGFloat>(h, 1.0));
  art->saturation = std::clamp(kSaturation * kColourfulArt / std::max(float(chroma / n), 0.01f),
                               kSaturation, kMaxSaturation);
  return art;
}

// XeniOS Green as art: the greens as soft lights over the deepest one.
XeniaAmbientArt* GreenArt() {
  static XeniaAmbientArt* art = nil;
  if (!art) {
    const CGFloat side = kArtSide;
    UIGraphicsImageRendererFormat* format = [UIGraphicsImageRendererFormat preferredFormat];
    format.scale = 1.0;
    UIGraphicsImageRenderer* renderer =
        [[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(side, side)
                                                format:format] autorelease];
    NSArray<UIColor*>* greens = GreenColours();
    UIImage* image = [renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
      [greens[2] setFill];
      UIRectFill(CGRectMake(0, 0, side, side));
      const CGPoint centres[] = {{0.25, 0.3}, {0.75, 0.4}, {0.4, 0.8}, {0.8, 0.85}};
      const int order[] = {0, 1, 3, 0};
      CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
      for (int i = 0; i < 4; ++i) {
        UIColor* colour = greens[order[i]];
        NSArray* colours = @[ (id)colour.CGColor, (id)[colour colorWithAlphaComponent:0].CGColor ];
        CGGradientRef gradient = CGGradientCreateWithColors(space, (CFArrayRef)colours, nullptr);
        const CGPoint c = CGPointMake(centres[i].x * side, centres[i].y * side);
        CGContextDrawRadialGradient(context.CGContext, gradient, c, 0, c, side * 0.45, 0);
        CGGradientRelease(gradient);
      }
      CGColorSpaceRelease(space);
    }];
    art = [MakeArt(image.CGImage) retain];
  }
  return art;
}

UIColor* Saturated(UIColor* colour) {
  CGFloat h = 0, s = 0, v = 0, a = 1;
  if (![colour getHue:&h saturation:&s brightness:&v alpha:&a]) {
    return colour;
  }
  return [UIColor colorWithHue:h
                    saturation:std::min<CGFloat>(1.0, s * kSaturation)
                    brightness:v
                         alpha:a];
}

bool IsDark(UITraitCollection* traits) {
  return traits.userInterfaceStyle == UIUserInterfaceStyleDark;
}

}  // namespace

XeniaLibraryBackground XeniaLibraryBackgroundSetting(void) {
  const NSInteger value =
      [[NSUserDefaults standardUserDefaults] integerForKey:kBackgroundDefaultsKey];
  return value >= XeniaLibraryBackgroundGameColours && value <= XeniaLibraryBackgroundOff
             ? XeniaLibraryBackground(value)
             : XeniaLibraryBackgroundGameColours;
}

void XeniaSetLibraryBackgroundSetting(XeniaLibraryBackground background) {
  [[NSUserDefaults standardUserDefaults] setInteger:background forKey:kBackgroundDefaultsKey];
  PostChange();
}

XeniaLibraryBackgroundMotion XeniaLibraryBackgroundMotionSetting(void) {
  return [[NSUserDefaults standardUserDefaults] integerForKey:kMotionDefaultsKey] ==
                 XeniaLibraryBackgroundMotionStill
             ? XeniaLibraryBackgroundMotionStill
             : XeniaLibraryBackgroundMotionSubtle;
}

void XeniaSetLibraryBackgroundMotionSetting(XeniaLibraryBackgroundMotion motion) {
  [[NSUserDefaults standardUserDefaults] setInteger:motion forKey:kMotionDefaultsKey];
  PostChange();
}

NSArray<UIColor*>* XeniaLibraryBackgroundColours(XeniaLibraryBackground background) {
  if (background == XeniaLibraryBackgroundGameColours && g_game_colours) {
    return g_game_colours;
  }
  return GreenColours();
}

UIImage* XeniaLibraryBackgroundSwatch(NSArray<UIColor*>* colours, CGFloat side) {
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(side, side)] autorelease];
  return [renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
    const CGRect rect = CGRectMake(0, 0, side, side);
    UIBezierPath* clip = [UIBezierPath bezierPathWithRoundedRect:rect cornerRadius:side * 0.24];
    [clip addClip];
    if (colours.count < 4) {
      [[UIColor colorWithWhite:0.5 alpha:0.18] setFill];
      UIRectFill(rect);
      [[UIColor colorWithWhite:0.5 alpha:0.35] setStroke];
      UIBezierPath* edge = [UIBezierPath bezierPathWithRoundedRect:CGRectInset(rect, 0.5, 0.5)
                                                      cornerRadius:side * 0.24];
      edge.lineWidth = 1.0;
      [edge stroke];
      return;
    }
    // The strongest colour across, the others at its ends.
    const CGFloat stops[] = {0.0, 0.5, 1.0};
    NSArray* gradient_colours = @[
      (id)Saturated(colours[3]).CGColor, (id)Saturated(colours[0]).CGColor,
      (id)Saturated(colours[2]).CGColor
    ];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef gradient = CGGradientCreateWithColors(space, (CFArrayRef)gradient_colours, stops);
    CGContextDrawLinearGradient(context.CGContext, gradient, CGPointMake(0, 0),
                                CGPointMake(side, side), 0);
    CGGradientRelease(gradient);
    CGColorSpaceRelease(space);
  }];
}

@implementation XeniaLibraryAmbientView {
  // The finished field, shown as a plain layer's contents rather than through a CAMetalLayer, so
  // tools that hook Metal presentation (the Metal Performance HUD) only see the game.
  CALayer* _surface;
  // Two IOSurface-backed targets taken in turn: one on screen while the other is drawn.
  id<MTLTexture> _outputs[2];
  int _shown;       // Which of _outputs the layer shows; -1 for neither yet.
  BOOL _inFlight;   // A frame is still on the GPU; the next waits for it.
  BOOL _drawAfter;  // A frame was asked for meanwhile; drawn when that one lands.
  CAGradientLayer* _veil;
  id<MTLCommandQueue> _queue;
  id<MTLRenderPipelineState> _piecePipeline;
  id<MTLRenderPipelineState> _blurPipeline;
  id<MTLRenderPipelineState> _finishPipeline;
  id<MTLRenderPipelineState> _anchorPipeline;
  // The Library's art, in this view, and the part of its image it shows; null for none.
  CGRect _anchorFrame;
  CGRect _anchorContents;
  // The copies, then the blur's halfway point; sized from the page.
  id<MTLTexture> _scene;
  id<MTLTexture> _blurred;
  XeniaAmbientArt* _fromArt;
  XeniaAmbientArt* _toArt;
  double _fade;  // 0 shows _fromArt, 1 _toArt.
  double _time;  // The field's clock; stands still while the field holds still.
  CADisplayLink* _link;
  CFTimeInterval _lastTick;
  BOOL _moving;
  NSString* _gameKey;
  NSTimer* _coverCheck;
  BOOL _watchesStyle;
}

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (!self) {
    return nil;
  }
  self.userInteractionEnabled = NO;
  self.isAccessibilityElement = NO;
  self.clipsToBounds = YES;
  self.backgroundColor = RGB(20, 24, 22);
  _fade = 1.0;
  _anchorFrame = CGRectNull;

  id<MTLDevice> device = Device();
  if (device) {
    _queue = [device newCommandQueue];
    _surface = [[CALayer layer] retain];
    _surface.opaque = YES;
    _surface.contentsGravity = kCAGravityResize;
    _surface.hidden = YES;  // Until its first frame.
    _shown = -1;
    [self.layer addSublayer:_surface];
    [self buildPipelines:device];
  }
  // A faint shade at the top and bottom keeps the header and grid readable.
  _veil = [[CAGradientLayer layer] retain];
  _veil.locations = @[ @0.0, @0.3, @1.0 ];
  [self.layer addSublayer:_veil];

  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  SEL update = @selector(updateRunning);
  [center addObserver:self
             selector:@selector(settingsChanged:)
                 name:XeniaLibraryBackgroundDidChangeNotification
               object:nil];
  for (NSNotificationName name in @[
         UIApplicationDidBecomeActiveNotification, UIApplicationWillResignActiveNotification,
         UIApplicationDidEnterBackgroundNotification,
         UIAccessibilityReduceMotionStatusDidChangeNotification
       ]) {
    [center addObserver:self selector:update name:name object:nil];
  }
  // Posted off the main thread.
  for (NSNotificationName name in @[
         NSProcessInfoPowerStateDidChangeNotification,
         NSProcessInfoThermalStateDidChangeNotification
       ]) {
    [center addObserver:self selector:@selector(updateRunningSoon) name:name object:nil];
  }
  [self applySetting];
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_link invalidate];
  [_link release];
  [_coverCheck invalidate];
  [_coverCheck release];
  [_surface release];
  [_outputs[0] release];
  [_outputs[1] release];
  [_veil release];
  [_queue release];
  [_piecePipeline release];
  [_blurPipeline release];
  [_finishPipeline release];
  [_anchorPipeline release];
  [_scene release];
  [_blurred release];
  [_fromArt release];
  [_toArt release];
  [_gameKey release];
  [super dealloc];
}

// Compiles the shaders off the main thread; the field draws from when they're ready.
- (void)buildPipelines:(id<MTLDevice>)device {
  [device newLibraryWithSource:kShaders
                       options:nil
             completionHandler:^(id<MTLLibrary> library, NSError* error) {
               if (!library) {
                 NSLog(@"Library background shaders didn't compile: %@", error);
                 return;
               }
               auto make = ^id<MTLRenderPipelineState>(NSString* vertex, NSString* fragment,
                                                       MTLPixelFormat format, BOOL blend) {
                 MTLRenderPipelineDescriptor* d =
                     [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
                 d.vertexFunction = [[library newFunctionWithName:vertex] autorelease];
                 d.fragmentFunction = [[library newFunctionWithName:fragment] autorelease];
                 MTLRenderPipelineColorAttachmentDescriptor* colour = d.colorAttachments[0];
                 colour.pixelFormat = format;
                 if (blend) {
                   colour.blendingEnabled = YES;
                   colour.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                   colour.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                   colour.sourceAlphaBlendFactor = MTLBlendFactorOne;
                   colour.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                 }
                 return [device newRenderPipelineStateWithDescriptor:d error:nullptr];
               };
               id<MTLRenderPipelineState> piece =
                   make(@"piece_vertex", @"piece_fragment", MTLPixelFormatRGBA16Float, NO);
               id<MTLRenderPipelineState> anchor =
                   make(@"anchor_vertex", @"anchor_fragment", MTLPixelFormatRGBA16Float, YES);
               id<MTLRenderPipelineState> blur =
                   make(@"screen_vertex", @"blur_fragment", MTLPixelFormatRGBA16Float, NO);
               id<MTLRenderPipelineState> finish =
                   make(@"screen_vertex", @"finish_fragment", MTLPixelFormatBGRA8Unorm, NO);
               dispatch_async(dispatch_get_main_queue(), ^{
                 if (piece && anchor && blur && finish) {
                   self->_piecePipeline = [piece retain];
                   self->_anchorPipeline = [anchor retain];
                   self->_blurPipeline = [blur retain];
                   self->_finishPipeline = [finish retain];
                   [self drawFrame];
                 }
                 [piece release];
                 [anchor release];
                 [blur release];
                 [finish release];
               });
             }];
}

- (BOOL)drawsBackground {
  return XeniaLibraryBackgroundSetting() != XeniaLibraryBackgroundOff;
}

- (void)settingsChanged:(NSNotification*)__unused notification {
  [self applySetting];
}

- (void)applySetting {
  const XeniaLibraryBackground background = XeniaLibraryBackgroundSetting();
  self.hidden = background == XeniaLibraryBackgroundOff;
  XeniaAmbientArt* art =
      background == XeniaLibraryBackgroundGameColours && g_game_art ? g_game_art : GreenArt();
  [self showArt:art];
  [self updateRunning];
}

- (void)setGameImage:(UIImage*)image key:(NSString*)key {
  if (!image || !key) {
    if (g_game_colours || g_game_art) {
      [g_game_colours release];
      g_game_colours = nil;
      [g_game_art release];
      g_game_art = nil;
      PostChange();
    }
    [_gameKey release];
    _gameKey = nil;
    return;
  }
  if ([_gameKey isEqualToString:key]) {
    return;
  }
  [_gameKey release];
  _gameKey = [key copy];
  NSArray* cached = [GameCache() objectForKey:key];
  if (cached) {
    [g_game_colours release];
    g_game_colours = [cached[0] retain];
    [g_game_art release];
    g_game_art = [cached[1] retain];
    PostChange();
    return;
  }
  CGImageRef cg = CGImageRetain(image.CGImage);
  if (!cg) {
    return;
  }
  NSString* wanted = [[key copy] autorelease];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    NSArray<UIColor*>* colours = ExtractColours(cg);
    XeniaAmbientArt* art = MakeArt(cg);
    CGImageRelease(cg);
    if (!colours || !art) {
      return;
    }
    NSArray* entry = @[ colours, art ];
    dispatch_async(dispatch_get_main_queue(), ^{
      [GameCache() setObject:entry forKey:wanted];
      if ([self->_gameKey isEqualToString:wanted]) {
        [g_game_colours release];
        g_game_colours = [entry[0] retain];
        [g_game_art release];
        g_game_art = [entry[1] retain];
        PostChange();
      }
    });
  });
}

- (void)setAnchorFrame:(CGRect)frame contentsRect:(CGRect)contents {
  if (CGRectEqualToRect(frame, _anchorFrame) && CGRectEqualToRect(contents, _anchorContents)) {
    return;
  }
  _anchorFrame = frame;
  _anchorContents = contents;
  if (!_link) {
    [self drawFrame];
  }
}

// ── Drawing ────────────────────────────────────────────────────────────────

// Fades to |art| over kArtFade while the Library can be seen; at once otherwise.
- (void)showArt:(XeniaAmbientArt*)art {
  if (!art || art == _toArt) {
    return;
  }
  const BOOL fade = _toArt && !self.hidden && [self librarySeen] &&
                    UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
  XeniaAmbientArt* from = fade ? _toArt : art;
  [from retain];
  [_fromArt release];
  _fromArt = from;
  [art retain];
  [_toArt release];
  _toArt = art;
  _fade = fade ? 0.0 : 1.0;
  // The page colour shows until the first frame, and wherever Metal can't draw.
  const simd_float3 b = art->base;
  self.backgroundColor = [UIColor colorWithRed:b.x * 0.6 green:b.y * 0.6 blue:b.z * 0.6 alpha:1.0];
  [self updateRunning];
  [self drawFrame];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGRect bounds = self.bounds;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  _surface.frame = bounds;
  _veil.frame = bounds;
  [CATransaction commit];
  [self drawFrame];
}

// Light or dark as the system has it: the Library itself is drawn dark over the field.
- (bool)systemDark {
  return IsDark(self.window ? self.window.traitCollection : self.traitCollection);
}

// The blur's two textures, for a page of |size| points.
- (BOOL)prepareTargets:(CGSize)size {
  const NSUInteger w = std::max<NSUInteger>(8, NSUInteger(std::lround(size.width * kSceneScale)));
  const NSUInteger h = std::max<NSUInteger>(8, NSUInteger(std::lround(size.height * kSceneScale)));
  if (_scene && _scene.width == w && _scene.height == h) {
    return YES;
  }
  [_scene release];
  [_blurred release];
  MTLTextureDescriptor* d =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                         width:w
                                                        height:h
                                                     mipmapped:NO];
  d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  d.storageMode = MTLStorageModePrivate;
  _scene = [Device() newTextureWithDescriptor:d];
  _blurred = [Device() newTextureWithDescriptor:d];
  return _scene && _blurred;
}

// The two targets the finished field is drawn into, |w| by |h| pixels.
- (BOOL)prepareOutputs:(NSUInteger)w height:(NSUInteger)h {
  if (_outputs[0] && _outputs[0].width == w && _outputs[0].height == h) {
    return YES;
  }
  MTLTextureDescriptor* d =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:w
                                                        height:h
                                                     mipmapped:NO];
  d.usage = MTLTextureUsageRenderTarget;
  d.storageMode = MTLStorageModeShared;
  NSDictionary* properties = @{
    (id)kIOSurfaceWidth : @(w),
    (id)kIOSurfaceHeight : @(h),
    (id)kIOSurfaceBytesPerElement : @4,
    (id)kIOSurfacePixelFormat : @(0x42475241u),  // 'BGRA'
  };
  for (int i = 0; i < 2; ++i) {
    [_outputs[i] release];
    _outputs[i] = nil;
    IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef)properties);
    if (!surface) {
      return NO;
    }
    _outputs[i] = [Device() newTextureWithDescriptor:d iosurface:surface plane:0];
    CFRelease(surface);
    if (!_outputs[i]) {
      return NO;
    }
  }
  // The layer may still show an old surface; it keeps it until the next frame replaces it.
  _shown = -1;
  return YES;
}

// One frame: the copies of the art, blurred across and down, then toned for the page.
- (void)drawFrame {
  const CGSize size = self.bounds.size;
  if (_inFlight) {
    _drawAfter = YES;
    return;
  }
  if (!_finishPipeline || !_toArt || self.hidden || !self.window || ![self launcherShown] ||
      size.width < 1.0 || size.height < 1.0 ||
      UIApplication.sharedApplication.applicationState == UIApplicationStateBackground ||
      ![self prepareTargets:size] ||
      ![self prepareOutputs:NSUInteger(std::max(8.0, std::round(size.width * kOutputScale)))
                     height:NSUInteger(std::max(8.0, std::round(size.height * kOutputScale)))]) {
    return;
  }
  const int next = _shown == 0 ? 1 : 0;
  id<MTLTexture> output = _outputs[next];
  const bool dark = [self systemDark];
  const double eased = _fade * _fade * (3.0 - 2.0 * _fade);
  Field field = {};
  field.time = float(_time);
  field.fade = float(eased);
  field.aspect = float(size.width / size.height);
  field.saturation =
      float(_fromArt->saturation + (_toArt->saturation - _fromArt->saturation) * eased);
  // White text stays readable over both: a deeper band in dark mode, a lighter one in light.
  // (OKLab lightness: about the same as 10–40% and 20–52% grey.)
  field.lo = dark ? 0.24f : 0.33f;
  field.hi = dark ? 0.52f : 0.61f;
  field.from_aspect = _fromArt->aspect;
  field.to_aspect = _toArt->aspect;
  // Only the game's own art lines up with the Library's.
  const BOOL anchored = !CGRectIsNull(_anchorFrame) && !CGRectIsEmpty(_anchorFrame) &&
                        _toArt == g_game_art && _fromArt == g_game_art;
  if (anchored) {
    field.anchor_rect = simd_make_float4(2.0 * _anchorFrame.origin.x / size.width - 1.0,
                                         1.0 - 2.0 * _anchorFrame.origin.y / size.height,
                                         2.0 * _anchorFrame.size.width / size.width,
                                         -2.0 * _anchorFrame.size.height / size.height);
    field.anchor_uv = simd_make_float4(_anchorContents.origin.x, _anchorContents.origin.y,
                                       _anchorContents.size.width, _anchorContents.size.height);
  }
  const simd_float3 base = simd_mix(_fromArt->base, _toArt->base, simd_make_float3(float(eased)));

  id<MTLCommandBuffer> commands = [_queue commandBuffer];
  auto pass = ^id<MTLRenderCommandEncoder>(id<MTLTexture> target, MTLLoadAction load) {
    MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = target;
    d.colorAttachments[0].loadAction = load;
    d.colorAttachments[0].clearColor = MTLClearColorMake(base.x, base.y, base.z, 1.0);
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    return [commands renderCommandEncoderWithDescriptor:d];
  };

  id<MTLRenderCommandEncoder> encoder = pass(_scene, MTLLoadActionClear);
  [encoder setRenderPipelineState:_piecePipeline];
  [encoder setVertexBytes:&field length:sizeof(field) atIndex:0];
  [encoder setVertexBytes:kPieces length:sizeof(kPieces) atIndex:1];
  [encoder setFragmentBytes:&field length:sizeof(field) atIndex:0];
  [encoder setFragmentTexture:_fromArt->texture atIndex:0];
  [encoder setFragmentTexture:_toArt->texture atIndex:1];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
              vertexStart:0
              vertexCount:4
            instanceCount:kPieceCount];
  if (anchored) {
    [encoder setRenderPipelineState:_anchorPipeline];
    [encoder setVertexBytes:&kAnchorReach length:sizeof(kAnchorReach) atIndex:1];
    [encoder setFragmentBytes:&kAnchorReach length:sizeof(kAnchorReach) atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
  }
  [encoder endEncoding];

  // Across, then down: about a tenth of the page wide.
  const simd_float2 steps[] = {simd_make_float2(1.5f / _scene.width, 0.0f),
                               simd_make_float2(0.0f, 1.5f / _scene.height)};
  id<MTLTexture> sources[] = {_scene, _blurred};
  id<MTLTexture> targets[] = {_blurred, _scene};
  for (int i = 0; i < 2; ++i) {
    encoder = pass(targets[i], MTLLoadActionDontCare);
    [encoder setRenderPipelineState:_blurPipeline];
    [encoder setFragmentBytes:&steps[i] length:sizeof(steps[i]) atIndex:0];
    [encoder setFragmentTexture:sources[i] atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
  }

  encoder = pass(output, MTLLoadActionDontCare);
  [encoder setRenderPipelineState:_finishPipeline];
  [encoder setFragmentBytes:&field length:sizeof(field) atIndex:0];
  [encoder setFragmentTexture:_scene atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  // Shown once the GPU is done with it, so the layer never shows a half-drawn surface.
  _inFlight = YES;
  [commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
    const BOOL drawn = done.status == MTLCommandBufferStatusCompleted;
    dispatch_async(dispatch_get_main_queue(), ^{
      self->_inFlight = NO;
      // Dropped if the targets were remade while it was on the GPU.
      if (drawn && self->_outputs[next] == output) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        self->_surface.contents = (id)output.iosurface;
        self->_surface.hidden = NO;
        [CATransaction commit];
        self->_shown = next;
      }
      if (self->_drawAfter) {
        self->_drawAfter = NO;
        [self drawFrame];
      }
    });
  }];
  [commands commit];

  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  UIColor* shade = UIColor.blackColor;
  _veil.colors = @[
    (id)[shade colorWithAlphaComponent:dark ? 0.28 : 0.18].CGColor,
    (id)[shade colorWithAlphaComponent:0.0].CGColor,
    (id)[shade colorWithAlphaComponent:dark ? 0.35 : 0.22].CGColor
  ];
  [CATransaction commit];
}

- (void)tick:(CADisplayLink*)link {
  const CFTimeInterval now = link.targetTimestamp;
  const double dt = _lastTick > 0.0 ? std::min(now - _lastTick, 0.1) : 0.0;
  _lastTick = now;
  if (_moving) {
    _time += dt * kSubtleSpeed;
  }
  _fade = std::min(1.0, _fade + dt / kArtFade);
  [self drawFrame];
  if (!_moving && _fade >= 1.0) {
    [self setTicking:NO];
  }
}

// Draws at up to 30 frames a second while on.
- (void)setTicking:(BOOL)ticking {
  if (ticking && !_link) {
    _link = [[CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)] retain];
    _link.preferredFrameRateRange = CAFrameRateRangeMake(10.0, 30.0, 30.0);
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    _lastTick = 0.0;
  } else if (!ticking && _link) {
    [_link invalidate];
    [_link release];
    _link = nil;
  }
}

// ── When it moves ──────────────────────────────────────────────────────────

- (void)didMoveToWindow {
  [super didMoveToWindow];
  if (self.window && !_watchesStyle) {
    _watchesStyle = YES;
    [self.window registerForTraitChanges:@[ [UITraitUserInterfaceStyle class] ]
                              withTarget:self
                                  action:@selector(systemStyleChanged)];
  }
  [self updateRunning];
  [self drawFrame];
}

- (void)systemStyleChanged {
  [self drawFrame];
}

- (void)visibilityMayHaveChanged {
  [self updateRunning];
  [self drawFrame];
}

- (void)updateRunningSoon {
  dispatch_async(dispatch_get_main_queue(), ^{
    [self updateRunning];
  });
}

// Whether the Library is on screen: in a window, shown, and not under another page.
- (BOOL)librarySeen {
  if (!self.window || self.hidden ||
      UIApplication.sharedApplication.applicationState == UIApplicationStateBackground) {
    return NO;
  }
  for (UIView* view = self.superview; view; view = view.superview) {
    if (view.hidden || view.alpha <= 0.01) {
      return NO;
    }
  }
  return self.window.rootViewController.presentedViewController == nil;
}

// Runs the clock while the Library can be seen and may move; draws only to finish a fade
// otherwise, and not at all while it's covered, a game runs or the app is away.
- (void)updateRunning {
  const BOOL seen = [self librarySeen] &&
                    UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
  NSProcessInfo* info = NSProcessInfo.processInfo;
  _moving = seen && XeniaLibraryBackgroundMotionSetting() == XeniaLibraryBackgroundMotionSubtle &&
            !UIAccessibilityIsReduceMotionEnabled() && !info.lowPowerModeEnabled &&
            info.thermalState < NSProcessInfoThermalStateSerious;
  if (!seen) {
    _fade = 1.0;
  }
  [self setTicking:_moving || (seen && _fade < 1.0)];
  // A page coming up over the Library isn't announced; look each second while the launcher is
  // up. Hidden for a game, the launcher says when it's back.
  const BOOL watch = self.window && !self.hidden && [self launcherShown];
  if (watch && !_coverCheck) {
    _coverCheck = [[NSTimer scheduledTimerWithTimeInterval:1.0
                                                    target:self
                                                  selector:@selector(updateRunning)
                                                  userInfo:nil
                                                   repeats:YES] retain];
    _coverCheck.tolerance = 0.5;
  } else if (!watch && _coverCheck) {
    [_coverCheck invalidate];
    [_coverCheck release];
    _coverCheck = nil;
  }
}

- (BOOL)launcherShown {
  for (UIView* view = self.superview; view; view = view.superview) {
    if (view.hidden) {
      return NO;
    }
  }
  return YES;
}

@end
