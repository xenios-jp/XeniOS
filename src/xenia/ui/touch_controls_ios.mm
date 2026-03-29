/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/touch_controls_ios.h"

#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>

namespace {

static UIColor* XeniaTouchOuterColor(void) {
  return [UIColor colorWithWhite:0.08f alpha:0.82f];
}

static UIColor* XeniaTouchInnerColor(void) {
  return [UIColor colorWithWhite:0.92f alpha:0.84f];
}

static UIColor* XeniaTouchPressedColor(void) {
  return [UIColor colorWithRed:0.42f green:0.87f blue:0.63f alpha:0.92f];
}

static UIColor* XeniaFaceButtonColor(NSInteger tag) {
  switch (tag) {
    case 9:
      return [UIColor colorWithRed:0.23f green:0.48f blue:0.96f alpha:0.92f];
    case 10:
      return [UIColor colorWithRed:0.96f green:0.82f blue:0.20f alpha:0.92f];
    case 11:
      return [UIColor colorWithRed:0.33f green:0.78f blue:0.33f alpha:0.92f];
    case 12:
      return [UIColor colorWithRed:0.91f green:0.28f blue:0.24f alpha:0.92f];
    default:
      return XeniaTouchInnerColor();
  }
}

static UIColor* XeniaDimmedFaceButtonColor(NSInteger tag) {
  switch (tag) {
    case 9:
      return [UIColor colorWithRed:0.17f green:0.36f blue:0.74f alpha:0.92f];
    case 10:
      return [UIColor colorWithRed:0.74f green:0.63f blue:0.15f alpha:0.92f];
    case 11:
      return [UIColor colorWithRed:0.23f green:0.56f blue:0.23f alpha:0.92f];
    case 12:
      return [UIColor colorWithRed:0.67f green:0.20f blue:0.17f alpha:0.92f];
    default:
      return XeniaTouchPressedColor();
  }
}

static UIColor* XeniaTouchCenterColor(void) {
  return [UIColor colorWithWhite:0.62f alpha:0.84f];
}

static UIColor* XeniaTouchMenuColor(void) {
  return [UIColor colorWithWhite:0.92f alpha:0.90f];
}

static UIColor* XeniaDarkenedColor(UIColor* color, CGFloat amount) {
  CGFloat red = 0.0f;
  CGFloat green = 0.0f;
  CGFloat blue = 0.0f;
  CGFloat alpha = 0.0f;
  if (![color getRed:&red green:&green blue:&blue alpha:&alpha]) {
    CGFloat white = 0.0f;
    if ([color getWhite:&white alpha:&alpha]) {
      red = white;
      green = white;
      blue = white;
    }
  }
  CGFloat multiplier = std::max<CGFloat>(0.0f, std::min<CGFloat>(1.0f, 1.0f - amount));
  return [UIColor colorWithRed:red * multiplier
                         green:green * multiplier
                          blue:blue * multiplier
                         alpha:alpha];
}

static BOOL XeniaColorHasVisibleFill(UIColor* color) {
  if (!color) {
    return NO;
  }
  CGFloat alpha = 0.0f;
  if ([color getRed:nil green:nil blue:nil alpha:&alpha]) {
    return alpha > 0.001f;
  }
  if ([color getWhite:nil alpha:&alpha]) {
    return alpha > 0.001f;
  }
  CGColorRef cg_color = color.CGColor;
  return cg_color ? CGColorGetAlpha(cg_color) > 0.001f : NO;
}

static NSDictionary* XeniaColorConfiguration(UIColor* color) {
  if (!color) {
    return nil;
  }
  CGFloat red = 0.0f;
  CGFloat green = 0.0f;
  CGFloat blue = 0.0f;
  CGFloat alpha = 0.0f;
  if (![color getRed:&red green:&green blue:&blue alpha:&alpha]) {
    CGFloat white = 0.0f;
    if ([color getWhite:&white alpha:&alpha]) {
      red = white;
      green = white;
      blue = white;
    } else {
      return nil;
    }
  }
  return @{
    @"r" : @(red),
    @"g" : @(green),
    @"b" : @(blue),
    @"a" : @(alpha),
  };
}

static UIColor* XeniaColorFromConfiguration(id configuration) {
  NSDictionary* dictionary = [configuration isKindOfClass:[NSDictionary class]] ? configuration : nil;
  if (!dictionary) {
    return nil;
  }
  NSNumber* red = [dictionary[@"r"] isKindOfClass:[NSNumber class]] ? dictionary[@"r"] : nil;
  NSNumber* green = [dictionary[@"g"] isKindOfClass:[NSNumber class]] ? dictionary[@"g"] : nil;
  NSNumber* blue = [dictionary[@"b"] isKindOfClass:[NSNumber class]] ? dictionary[@"b"] : nil;
  NSNumber* alpha = [dictionary[@"a"] isKindOfClass:[NSNumber class]] ? dictionary[@"a"] : nil;
  if (!red || !green || !blue || !alpha) {
    return nil;
  }
  return [UIColor colorWithRed:(CGFloat)red.doubleValue
                         green:(CGFloat)green.doubleValue
                          blue:(CGFloat)blue.doubleValue
                         alpha:(CGFloat)alpha.doubleValue];
}

static CGPoint XeniaTouchLocation(UITouch* touch, UIView* view) {
  return [touch preciseLocationInView:view];
}

static constexpr CGFloat kXeniaRightTouchpadSensitivity = 2000.0f;

static NSDictionary* XeniaDisabledLayerActions(void) {
  static NSDictionary* actions = nil;
  static dispatch_once_t once_token;
  dispatch_once(&once_token, ^{
    actions = [[NSDictionary alloc] initWithObjectsAndKeys:
        [NSNull null], @"bounds",
        [NSNull null], @"contents",
        [NSNull null], @"frame",
        [NSNull null], @"hidden",
        [NSNull null], @"opacity",
        [NSNull null], @"position",
        [NSNull null], @"transform",
        nil];
  });
  return actions;
}

static void XeniaPrepareLayer(CALayer* layer) {
  layer.actions = XeniaDisabledLayerActions();
  layer.contentsGravity = kCAGravityResize;
  layer.contentsScale = UIScreen.mainScreen.scale;
  layer.drawsAsynchronously = YES;
  layer.shouldRasterize = NO;
  layer.rasterizationScale = UIScreen.mainScreen.scale;
  layer.opaque = NO;
  layer.anchorPoint = CGPointMake(0.5f, 0.5f);
}

static int16_t XeniaThumbAxis(CGFloat value) {
  value = std::max<CGFloat>(-1.0f, std::min<CGFloat>(1.0f, value));
  if (value >= 1.0f) {
    return INT16_MAX;
  }
  if (value <= -1.0f) {
    return INT16_MIN;
  }
  return static_cast<int16_t>(std::lrint(value * 32767.0f));
}

class ThumbVector {
 public:
  ThumbVector() = default;
  ThumbVector(CGFloat x, CGFloat y) : x(x), y(y) {}
  CGFloat x = 0.0f;
  CGFloat y = 0.0f;
};

struct XeniaTouchDefaultLayout {
  CGRect button_frames[15];
  CGRect menu_frame = CGRectZero;
  CGRect thumb_frames[2];
};

static constexpr NSInteger kXeniaTouchControlIdentifierMenu = 100;
static constexpr NSInteger kXeniaTouchControlIdentifierLeftStick = 101;
static constexpr NSInteger kXeniaTouchControlIdentifierRightStick = 102;

static CGFloat XeniaClamp(CGFloat value, CGFloat minimum, CGFloat maximum) {
  return std::max(minimum, std::min(maximum, value));
}

static XeniaTouchDefaultLayout XeniaBuildDefaultTouchLayout(CGFloat width, CGFloat height,
                                                            UIEdgeInsets insets, BOOL is_pad) {
  XeniaTouchDefaultLayout layout = {};
  CGFloat short_side = MIN(width, height);

  CGFloat main_button_size = short_side * (is_pad ? 0.098f : 0.136f);
  main_button_size = MAX(54.0f, MIN(main_button_size, 88.0f));
  CGFloat small_button_size = roundf(main_button_size * 0.72f);
  CGFloat cluster_gap = roundf(main_button_size * (is_pad ? 0.44f : 0.40f));
  CGFloat stick_touch_size = roundf(main_button_size * (is_pad ? 1.40f : 1.24f));

  CGFloat horizontal_edge_padding = short_side * (is_pad ? 0.055f : 0.040f);
  CGFloat bottom_padding = 0.0f;
  CGFloat control_inset = roundf(short_side * (is_pad ? 0.022f : 0.018f));
  CGFloat shoulder_width = roundf(main_button_size * 1.14f);
  CGFloat shoulder_height = roundf(main_button_size * 0.82f);
  CGFloat trigger_height = roundf(main_button_size * 1.50f);
  CGFloat shoulder_gap = roundf(main_button_size * 0.22f);
  CGFloat top_margin = is_pad ? (insets.top + short_side * 0.092f) : insets.top;
  CGFloat side_margin = insets.left + short_side * (is_pad ? 0.060f : 0.050f) + control_inset;

  layout.button_frames[1] = CGRectMake(side_margin, top_margin, shoulder_width, trigger_height);
  layout.button_frames[2] = CGRectMake(CGRectGetMaxX(layout.button_frames[1]) + shoulder_gap,
                                       top_margin, shoulder_width, shoulder_height);
  layout.button_frames[4] =
      CGRectMake(width - insets.right - (side_margin - insets.left) - shoulder_width, top_margin,
                 shoulder_width, trigger_height);
  layout.button_frames[3] =
      CGRectMake(CGRectGetMinX(layout.button_frames[4]) - shoulder_gap - shoulder_width,
                 top_margin, shoulder_width, shoulder_height);

  CGFloat left_cluster_center_x =
      insets.left + horizontal_edge_padding + main_button_size + cluster_gap + control_inset;
  CGFloat right_cluster_center_x =
      width - insets.right - horizontal_edge_padding - main_button_size - cluster_gap - control_inset;
  CGFloat stick_cluster_center_y =
      height - insets.bottom - bottom_padding - main_button_size - cluster_gap - (is_pad ? 52.0f : 0.0f);
  CGFloat button_cluster_center_y = stick_cluster_center_y;

  layout.thumb_frames[0] = CGRectMake(left_cluster_center_x - stick_touch_size * 0.5f,
                                      stick_cluster_center_y - stick_touch_size * 0.5f,
                                      stick_touch_size, stick_touch_size);
  layout.thumb_frames[1] = CGRectMake(right_cluster_center_x - stick_touch_size * 0.5f,
                                      stick_cluster_center_y - stick_touch_size * 0.5f,
                                      stick_touch_size, stick_touch_size);

  left_cluster_center_x = CGRectGetMidX(layout.thumb_frames[0]);
  right_cluster_center_x = CGRectGetMidX(layout.thumb_frames[1]);

  layout.button_frames[5] = CGRectMake(left_cluster_center_x - main_button_size * 0.5f,
                                       button_cluster_center_y - main_button_size - cluster_gap,
                                       main_button_size, main_button_size);
  layout.button_frames[6] = CGRectMake(left_cluster_center_x - main_button_size * 0.5f,
                                       button_cluster_center_y + cluster_gap, main_button_size,
                                       main_button_size);
  layout.button_frames[7] = CGRectMake(left_cluster_center_x - main_button_size - cluster_gap,
                                       button_cluster_center_y - main_button_size * 0.5f,
                                       main_button_size, main_button_size);
  layout.button_frames[8] = CGRectMake(left_cluster_center_x + cluster_gap,
                                       button_cluster_center_y - main_button_size * 0.5f,
                                       main_button_size, main_button_size);

  layout.button_frames[10] = CGRectMake(right_cluster_center_x - main_button_size * 0.5f,
                                        button_cluster_center_y - main_button_size - cluster_gap,
                                        main_button_size, main_button_size);
  layout.button_frames[11] = CGRectMake(right_cluster_center_x - main_button_size * 0.5f,
                                        button_cluster_center_y + cluster_gap, main_button_size,
                                        main_button_size);
  layout.button_frames[9] = CGRectMake(right_cluster_center_x - main_button_size - cluster_gap,
                                       button_cluster_center_y - main_button_size * 0.5f,
                                       main_button_size, main_button_size);
  layout.button_frames[12] = CGRectMake(right_cluster_center_x + cluster_gap,
                                        button_cluster_center_y - main_button_size * 0.5f,
                                        main_button_size, main_button_size);

  CGFloat menu_size = roundf(main_button_size * 0.78f);
  CGFloat menu_y = height - menu_size;
  CGFloat bottom_button_gap = roundf(small_button_size * 0.24f);
  CGFloat menu_center_y = menu_y + menu_size * 0.5f;

  layout.button_frames[13] =
      CGRectMake(width * 0.5f - menu_size * 0.5f - bottom_button_gap - small_button_size,
                 menu_center_y - small_button_size * 0.5f, small_button_size, small_button_size);
  layout.button_frames[14] =
      CGRectMake(width * 0.5f + menu_size * 0.5f + bottom_button_gap,
                 menu_center_y - small_button_size * 0.5f, small_button_size, small_button_size);
  layout.menu_frame = CGRectMake(width * 0.5f - menu_size * 0.5f, menu_y, menu_size, menu_size);
  return layout;
}

static CGRect XeniaScaledRect(CGRect rect, CGFloat scale) {
  CGFloat clamped_scale = XeniaClamp(scale, 0.6f, 5.0f);
  CGPoint center = CGPointMake(CGRectGetMidX(rect), CGRectGetMidY(rect));
  CGSize size = CGSizeMake(CGRectGetWidth(rect) * clamped_scale, CGRectGetHeight(rect) * clamped_scale);
  return CGRectMake(center.x - size.width * 0.5f, center.y - size.height * 0.5f, size.width, size.height);
}

static CGRect XeniaClampRectToBounds(CGRect rect, CGRect bounds, UIEdgeInsets insets) {
  CGRect available = UIEdgeInsetsInsetRect(bounds, insets);
  if (CGRectIsEmpty(available) || available.size.width <= 0.0f || available.size.height <= 0.0f) {
    available = bounds;
  }
  if (CGRectGetWidth(rect) > CGRectGetWidth(available)) {
    rect.size.width = CGRectGetWidth(available);
    rect.origin.x = CGRectGetMinX(available);
  } else {
    rect.origin.x =
        XeniaClamp(rect.origin.x, CGRectGetMinX(available), CGRectGetMaxX(available) - CGRectGetWidth(rect));
  }
  if (CGRectGetHeight(rect) > CGRectGetHeight(available)) {
    rect.size.height = CGRectGetHeight(available);
    rect.origin.y = CGRectGetMinY(available);
  } else {
    rect.origin.y = XeniaClamp(rect.origin.y, CGRectGetMinY(available),
                               CGRectGetMaxY(available) - CGRectGetHeight(rect));
  }
  return rect;
}

static NSNumber* XeniaNumberOrNil(CGFloat value) { return [NSNumber numberWithDouble:value]; }

static CGFloat XeniaThumbRadius(CGRect frame) {
  return MIN(CGRectGetWidth(frame), CGRectGetHeight(frame)) * 0.46f;
}

static CGSize XeniaThumbKnobSize(CGRect frame, XeniaTouchControlShape shape,
                                 XeniaTouchStickInputMode input_mode) {
  CGFloat base = MIN(CGRectGetWidth(frame), CGRectGetHeight(frame));
  CGFloat width = base * (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad ? 0.52f : 0.54f);
  CGFloat height = width;
  if (shape == XeniaTouchControlShapeRoundedRectangle) {
    if (input_mode == XeniaTouchStickInputModeTouchpad) {
      width = CGRectGetWidth(frame) * 0.44f;
      height = CGRectGetHeight(frame) * 0.28f;
    } else {
      width = CGRectGetWidth(frame) * 0.58f;
      height = CGRectGetHeight(frame) * 0.40f;
    }
  } else if (shape == XeniaTouchControlShapeRoundedSquare) {
    CGFloat side = MIN(CGRectGetWidth(frame), CGRectGetHeight(frame)) *
                   (input_mode == XeniaTouchStickInputModeTouchpad ? 0.38f : 0.48f);
    width = side;
    height = side;
  } else if (input_mode == XeniaTouchStickInputModeTouchpad) {
    CGFloat side = MIN(CGRectGetWidth(frame), CGRectGetHeight(frame)) * 0.34f;
    width = side;
    height = side;
  }
  return CGSizeMake(width, height);
}

static CGPoint XeniaThumbCenter(CGRect frame) {
  return CGPointMake(CGRectGetMidX(frame), CGRectGetMidY(frame));
}

static CGRect XeniaThumbKnobRect(CGRect frame, const ThumbVector& vector,
                                 XeniaTouchControlShape shape,
                                 XeniaTouchStickInputMode input_mode) {
  CGSize knob_size = XeniaThumbKnobSize(frame, shape, input_mode);
  CGFloat radius_x = 0.0f;
  CGFloat radius_y = 0.0f;
  if (input_mode == XeniaTouchStickInputModeJoystick) {
    CGFloat radius = XeniaThumbRadius(frame);
    radius_x = radius;
    radius_y = radius;
  } else {
    radius_x = MAX((CGRectGetWidth(frame) - knob_size.width) * 0.34f, 0.0f);
    radius_y = MAX((CGRectGetHeight(frame) - knob_size.height) * 0.34f, 0.0f);
  }
  CGPoint center = XeniaThumbCenter(frame);
  CGPoint knob_center = CGPointMake(center.x + vector.x * radius_x, center.y - vector.y * radius_y);
  return CGRectMake(knob_center.x - knob_size.width * 0.5f,
                    knob_center.y - knob_size.height * 0.5f,
                    knob_size.width,
                    knob_size.height);
}

static UIFont* XeniaFont(CGFloat size, UIFontWeight weight) {
  return [UIFont systemFontOfSize:size weight:weight];
}

static NSString* XeniaButtonTitle(NSInteger tag) {
  switch (tag) {
    case 1: return @"LT";
    case 2: return @"LB";
    case 3: return @"RB";
    case 4: return @"RT";
    case 5: return @"▲";
    case 6: return @"▼";
    case 7: return @"◀";
    case 8: return @"▶";
    case 9: return @"X";
    case 10: return @"Y";
    case 11: return @"A";
    case 12: return @"B";
    case 13: return @"◀";
    case 14: return @"▶";
    default: return @"";
  }
}

static NSString* XeniaDefaultTitleForControlIdentifier(NSInteger controlIdentifier) {
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    return XeniaButtonTitle(controlIdentifier);
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    return @"⎋";
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick) {
    return @"LS";
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    return @"RS";
  }
  return @"";
}

static UIFont* XeniaButtonFontForTag(NSInteger tag) {
  switch (tag) {
    case 1:
    case 2:
    case 3:
    case 4:
      return XeniaFont(24.0f, UIFontWeightSemibold);
    case 13:
    case 14:
      return XeniaFont(24.0f, UIFontWeightBold);
    default:
      return XeniaFont(30.0f, UIFontWeightSemibold);
  }
}

static UIColor* XeniaButtonFillColor(NSInteger tag, BOOL pressed) {
  BOOL is_face_button = tag >= 9 && tag <= 12;
  UIColor* default_color = XeniaFaceButtonColor(tag);
  return is_face_button ? (pressed ? XeniaDimmedFaceButtonColor(tag) : default_color)
                        : (pressed ? XeniaTouchPressedColor() : default_color);
}

static UIColor* XeniaDefaultColorForControlIdentifier(NSInteger controlIdentifier, BOOL pressed) {
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    return XeniaButtonFillColor(controlIdentifier, pressed);
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    return pressed ? XeniaTouchPressedColor() : XeniaTouchMenuColor();
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick ||
      controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    return pressed ? XeniaDarkenedColor(XeniaTouchCenterColor(), 0.18f) : XeniaTouchCenterColor();
  }
  return pressed ? XeniaTouchPressedColor() : XeniaTouchInnerColor();
}

static void XeniaDrawTextInRect(NSString* text, CGRect rect, UIFont* font, UIColor* color,
                                CGFloat kern = 0.0f) {
  if (!text.length) {
    return;
  }
  NSMutableParagraphStyle* style = [[[NSMutableParagraphStyle alloc] init] autorelease];
  style.alignment = NSTextAlignmentCenter;
  NSMutableDictionary* attributes = [@{
    NSFontAttributeName : font,
    NSForegroundColorAttributeName : color,
    NSParagraphStyleAttributeName : style,
  } mutableCopy];
  if (fabs(kern) > 0.001f) {
    attributes[NSKernAttributeName] = @(kern);
  }
  CGRect text_bounds = [text boundingRectWithSize:CGSizeMake(CGRectGetWidth(rect), CGFLOAT_MAX)
                                          options:NSStringDrawingUsesLineFragmentOrigin |
                                                  NSStringDrawingUsesFontLeading
                                       attributes:attributes
                                          context:nil];
  CGRect text_rect = CGRectMake(CGRectGetMinX(rect),
                                CGRectGetMidY(rect) - CGRectGetHeight(text_bounds) * 0.5f,
                                CGRectGetWidth(rect),
                                CGRectGetHeight(text_bounds));
  [text drawInRect:CGRectIntegral(text_rect) withAttributes:attributes];
  [attributes release];
}

static UIFont* XeniaStickLabelFont(CGRect rect, NSString* title) {
  CGFloat side = MIN(CGRectGetWidth(rect), CGRectGetHeight(rect));
  CGFloat font_size = side * 0.28f;
  if (title.length >= 2) {
    font_size = side * 0.25f;
  }
  return XeniaFont(MAX(14.0f, MIN(font_size, 19.0f)), UIFontWeightSemibold);
}

static CGFloat XeniaStickLabelKern(CGRect rect, NSString* title) {
  if (title.length < 2) {
    return 0.0f;
  }
  CGFloat side = MIN(CGRectGetWidth(rect), CGRectGetHeight(rect));
  return MAX(0.4f, MIN(side * 0.012f, 1.0f));
}

static void XeniaDrawCircleButton(CGRect rect, UIColor* fill, NSString* title, UIFont* font) {
  CGContextRef ctx = UIGraphicsGetCurrentContext();
  if (XeniaColorHasVisibleFill(fill)) {
    CGContextSetFillColorWithColor(ctx, fill.CGColor);
    CGContextFillEllipseInRect(ctx, rect);
  }
  CGContextSetStrokeColorWithColor(ctx, XeniaTouchOuterColor().CGColor);
  CGContextSetLineWidth(ctx, 7.0f);
  CGContextStrokeEllipseInRect(ctx, CGRectInset(rect, 3.5f, 3.5f));
  XeniaDrawTextInRect(title, rect, font, [UIColor colorWithWhite:0.17f alpha:1.0f]);
}

static void XeniaDrawRoundedButton(CGRect rect, UIColor* fill, NSString* title, UIFont* font,
                                   CGFloat radius) {
  constexpr CGFloat kStrokeWidth = 7.0f;
  CGRect stroke_rect = CGRectInset(rect, kStrokeWidth * 0.5f, kStrokeWidth * 0.5f);
  CGFloat stroke_radius = MAX(radius - kStrokeWidth * 0.5f, 0.0f);
  UIBezierPath* fill_path = [UIBezierPath bezierPathWithRoundedRect:rect cornerRadius:radius];
  UIBezierPath* stroke_path =
      [UIBezierPath bezierPathWithRoundedRect:stroke_rect cornerRadius:stroke_radius];
  if (XeniaColorHasVisibleFill(fill)) {
    [fill setFill];
    [fill_path fill];
  }
  stroke_path.lineWidth = kStrokeWidth;
  stroke_path.lineJoinStyle = kCGLineJoinRound;
  [XeniaTouchOuterColor() setStroke];
  [stroke_path stroke];
  XeniaDrawTextInRect(title, rect, font, [UIColor colorWithWhite:0.17f alpha:1.0f]);
}

static void XeniaDrawMenuButton(CGRect rect, BOOL pressed) {
  UIColor* fill = pressed ? XeniaTouchPressedColor() : XeniaTouchMenuColor();
  CGContextRef ctx = UIGraphicsGetCurrentContext();
  CGContextSetFillColorWithColor(ctx, fill.CGColor);
  CGContextFillEllipseInRect(ctx, rect);
  CGContextSetStrokeColorWithColor(ctx, XeniaTouchOuterColor().CGColor);
  CGContextSetLineWidth(ctx, 6.0f);
  CGContextStrokeEllipseInRect(ctx, CGRectInset(rect, 3.0f, 3.0f));
  XeniaDrawTextInRect(@"⎋", rect, XeniaFont(26.0f, UIFontWeightBold),
                      [UIColor colorWithWhite:0.14f alpha:1.0f]);
}

static void XeniaDrawThumbKnob(CGRect rect, NSString* title) {
  CGContextRef ctx = UIGraphicsGetCurrentContext();
  CGContextSetFillColorWithColor(ctx, XeniaTouchCenterColor().CGColor);
  CGContextFillEllipseInRect(ctx, rect);
  CGContextSetStrokeColorWithColor(ctx, XeniaTouchOuterColor().CGColor);
  CGContextSetLineWidth(ctx, 6.0f);
  CGContextStrokeEllipseInRect(ctx, CGRectInset(rect, 3.0f, 3.0f));
  XeniaDrawTextInRect(title, rect, XeniaStickLabelFont(rect, title),
                      [UIColor colorWithWhite:0.14f alpha:1.0f],
                      XeniaStickLabelKern(rect, title));
}

static UIImage* XeniaRenderImage(CGSize size, void (^draw_block)(CGRect rect)) {
  if (size.width <= 0.0f || size.height <= 0.0f) {
    return nil;
  }
  UIGraphicsBeginImageContextWithOptions(size, NO, 0.0f);
  CGRect rect = CGRectMake(0.0f, 0.0f, size.width, size.height);
  draw_block(rect);
  UIImage* image = UIGraphicsGetImageFromCurrentImageContext();
  UIGraphicsEndImageContext();
  return image;
}

static UIImage* XeniaCreateButtonImage(NSInteger tag, CGSize size, BOOL pressed, NSString* title, UIColor* fill) {
  UIFont* font = XeniaButtonFontForTag(tag);
  if (tag <= 4) {
    return XeniaRenderImage(size, ^(CGRect rect) {
      XeniaDrawRoundedButton(rect, fill, title, font, roundf(CGRectGetHeight(rect) * 0.32f));
    });
  }
  return XeniaRenderImage(size, ^(CGRect rect) {
    XeniaDrawCircleButton(rect, fill, title, font);
  });
}

static UIImage* XeniaCreateMenuImage(CGSize size, BOOL pressed, NSString* title, UIColor* fill) {
  return XeniaRenderImage(size, ^(CGRect rect) {
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    CGContextSetFillColorWithColor(ctx, (fill ?: (pressed ? XeniaTouchPressedColor() : XeniaTouchMenuColor())).CGColor);
    CGContextFillEllipseInRect(ctx, rect);
    CGContextSetStrokeColorWithColor(ctx, XeniaTouchOuterColor().CGColor);
    CGContextSetLineWidth(ctx, 6.0f);
    CGContextStrokeEllipseInRect(ctx, CGRectInset(rect, 3.0f, 3.0f));
    XeniaDrawTextInRect(title ?: @"⎋", rect, XeniaFont(26.0f, UIFontWeightBold),
                        [UIColor colorWithWhite:0.14f alpha:1.0f]);
  });
}

static UIImage* XeniaCreateThumbImage(CGSize size, NSString* title, UIColor* fill) {
  return XeniaRenderImage(size, ^(CGRect rect) {
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    UIColor* resolved_fill = fill ?: XeniaTouchCenterColor();
    if (XeniaColorHasVisibleFill(resolved_fill)) {
      CGContextSetFillColorWithColor(ctx, resolved_fill.CGColor);
      CGContextFillEllipseInRect(ctx, rect);
    }
    CGContextSetStrokeColorWithColor(ctx, XeniaTouchOuterColor().CGColor);
    CGContextSetLineWidth(ctx, 6.0f);
    CGContextStrokeEllipseInRect(ctx, CGRectInset(rect, 3.0f, 3.0f));
    XeniaDrawTextInRect(title, rect, XeniaStickLabelFont(rect, title),
                        [UIColor colorWithWhite:0.14f alpha:1.0f],
                        XeniaStickLabelKern(rect, title));
  });
}

static XeniaTouchControlShape XeniaDefaultShapeForControlIdentifier(NSInteger controlIdentifier) {
  if (controlIdentifier >= 1 && controlIdentifier <= 4) {
    return XeniaTouchControlShapeRoundedRectangle;
  }
  return XeniaTouchControlShapeCircle;
}

static CGRect XeniaRectForShape(CGRect rect, XeniaTouchControlShape shape) {
  if (shape != XeniaTouchControlShapeRoundedSquare) {
    return rect;
  }
  CGFloat side = MIN(CGRectGetWidth(rect), CGRectGetHeight(rect));
  CGPoint center = CGPointMake(CGRectGetMidX(rect), CGRectGetMidY(rect));
  return CGRectMake(center.x - side * 0.5f, center.y - side * 0.5f, side, side);
}

static CGRect XeniaDefaultFrameAdjustedForShape(CGRect rect, NSInteger controlIdentifier,
                                                XeniaTouchControlShape shape) {
  CGPoint center = CGPointMake(CGRectGetMidX(rect), CGRectGetMidY(rect));
  CGFloat width = CGRectGetWidth(rect);
  CGFloat height = CGRectGetHeight(rect);
  switch (shape) {
    case XeniaTouchControlShapeCircle:
    case XeniaTouchControlShapeRoundedSquare: {
      CGFloat side = sqrtf(MAX(width * height, 0.0f));
      return CGRectMake(center.x - side * 0.5f, center.y - side * 0.5f, side, side);
    }
    case XeniaTouchControlShapeRoundedRectangle:
    default: {
      if (fabs(width - height) > 1.0f) {
        return rect;
      }
      CGFloat side = MIN(width, height);
      CGFloat rect_width = side * 1.18f;
      CGFloat rect_height = side * 0.90f;
      if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick ||
          controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
        rect_width = side * 1.42f;
        rect_height = side * 0.78f;
      }
      return CGRectMake(center.x - rect_width * 0.5f, center.y - rect_height * 0.5f, rect_width,
                        rect_height);
    }
  }
}

static UIImage* XeniaCreateControlImage(CGSize size, NSInteger controlIdentifier, BOOL pressed,
                                        NSString* title, UIColor* fill,
                                        XeniaTouchControlShape shape) {
  UIFont* font = controlIdentifier == kXeniaTouchControlIdentifierMenu
                     ? XeniaFont(26.0f, UIFontWeightBold)
                     : (controlIdentifier == kXeniaTouchControlIdentifierLeftStick ||
                                controlIdentifier == kXeniaTouchControlIdentifierRightStick
                            ? XeniaStickLabelFont(CGRectMake(0.0f, 0.0f, size.width, size.height),
                                                  title ?: XeniaDefaultTitleForControlIdentifier(
                                                               controlIdentifier))
                            : XeniaButtonFontForTag(controlIdentifier));
  UIColor* resolved_fill = fill ?: XeniaDefaultColorForControlIdentifier(controlIdentifier, pressed);
  NSString* resolved_title = title ?: XeniaDefaultTitleForControlIdentifier(controlIdentifier);
  return XeniaRenderImage(size, ^(CGRect rect) {
    CGRect shaped_rect = XeniaRectForShape(rect, shape);
    switch (shape) {
      case XeniaTouchControlShapeRoundedRectangle:
        XeniaDrawRoundedButton(shaped_rect, resolved_fill, resolved_title, font,
                               roundf(CGRectGetHeight(shaped_rect) * 0.32f));
        break;
      case XeniaTouchControlShapeRoundedSquare:
        XeniaDrawRoundedButton(shaped_rect, resolved_fill, resolved_title, font,
                               roundf(CGRectGetWidth(shaped_rect) * 0.24f));
        break;
      case XeniaTouchControlShapeCircle:
      default:
        XeniaDrawCircleButton(shaped_rect, resolved_fill, resolved_title, font);
        break;
    }
  });
}

static void XeniaDrawImageInRect(CGContextRef ctx, UIImage* image, CGRect rect, CGFloat rotation) {
  if (!image || CGRectIsEmpty(rect)) {
    return;
  }
  CGContextSaveGState(ctx);
  CGPoint center = CGPointMake(CGRectGetMidX(rect), CGRectGetMidY(rect));
  CGContextTranslateCTM(ctx, center.x, center.y);
  if (fabs(rotation) > 0.0001f) {
    CGContextRotateCTM(ctx, rotation);
  }
  [image drawInRect:CGRectMake(-CGRectGetWidth(rect) * 0.5f, -CGRectGetHeight(rect) * 0.5f,
                               CGRectGetWidth(rect), CGRectGetHeight(rect))];
  CGContextRestoreGState(ctx);
}

}  // namespace

@interface XeniaTouchControlsView () {
 @private
  xe::hid::X_INPUT_STATE state_;
  CGRect button_frames_[15];
  CGRect menu_frame_;
  CGRect thumb_frames_[2];
  ThumbVector thumb_vectors_[2];
  BOOL button_pressed_[15];
  BOOL menu_pressed_;
  UITouch* button_touches_[15];
  UITouch* thumb_touches_[2];
  CGPoint thumb_touch_start_points_[2];
  CGPoint thumb_touch_last_points_[2];
  BOOL thumb_touch_dragged_[2];
  uint32_t thumb_touchpad_activity_tokens_[2];
  UITouch* menu_touch_;
  UITouch* layout_edit_touch_;
  CGPoint layout_edit_last_point_;
  CGPoint layout_edit_touch_start_point_;
  NSInteger layout_edit_touch_start_control_identifier_;
  NSInteger layout_edit_previous_selected_control_identifier_;
  BOOL layout_edit_touch_moved_;
  CGSize button_image_sizes_[15];
  UIImage* button_images_[15][2];
  CGSize menu_image_size_;
  UIImage* menu_images_[2];
  CGSize knob_image_sizes_[2];
  UIImage* knob_images_[2];
  CGSize thumb_base_image_sizes_[2];
  UIImage* thumb_base_images_[2];
  CGSize gameplay_controls_image_size_;
  UIImage* gameplay_controls_image_;
  CALayer* gameplay_controls_layer_;
  CALayer* button_layers_[15];
  CALayer* menu_layer_;
  CALayer* thumb_base_layers_[2];
  CALayer* thumb_layers_[2];
  CGPoint button_offset_units_[15];
  CGFloat button_scales_[15];
  CGFloat button_rotations_[15];
  NSInteger button_shapes_[15];
  BOOL button_hidden_[15];
  NSString* button_titles_[15];
  UIColor* button_colors_[15];
  CGPoint thumb_offset_units_[2];
  CGFloat thumb_scales_[2];
  CGFloat thumb_rotations_[2];
  NSInteger thumb_shapes_[2];
  NSInteger thumb_input_modes_[2];
  BOOL thumb_hidden_[2];
  NSString* thumb_titles_[2];
  UIColor* thumb_colors_[2];
  CGPoint menu_offset_units_;
  CGFloat menu_scale_;
  CGFloat menu_rotation_;
  NSInteger menu_shape_;
  BOOL menu_hidden_;
  NSString* menu_title_;
  UIColor* menu_color_;
  BOOL layoutEditingEnabled_;
  NSInteger selectedControlIdentifier_;
  UIRotationGestureRecognizer* layout_rotation_gesture_;
  void (^stateDidChangeHandler_)(void);
  void (^menuButtonTappedHandler_)(void);
  void (^layoutEditingSelectionDidChangeHandler_)(NSInteger);
  void (^layoutEditingDidChangeHandler_)(void);
}
- (void)rebuildAssetsIfNeededForButtonTag:(NSInteger)tag size:(CGSize)size;
- (void)rebuildMenuAssetsIfNeeded:(CGSize)size;
- (void)rebuildThumbBaseAssetIfNeededForIndex:(NSInteger)index size:(CGSize)size;
- (void)rebuildKnobAssetIfNeededForIndex:(NSInteger)index size:(CGSize)size;
- (void)invalidateButtonAssetsForTag:(NSInteger)tag;
- (void)invalidateMenuAssets;
- (void)invalidateKnobAssets;
- (void)invalidateGameplayControlsComposite;
- (void)rebuildGameplayControlsCompositeIfNeeded;
- (void)updateGameplayControlsLayer;
- (void)updateButtonLayerForTag:(NSInteger)tag;
- (void)updateMenuLayer;
- (CGRect)thumbKnobRectForIndex:(NSInteger)index;
- (CGRect)thumbHitRectForIndex:(NSInteger)index;
- (void)updateThumbBaseLayerForIndex:(NSInteger)index;
- (void)updateThumbLayerForIndex:(NSInteger)index;
- (void)setButtonTag:(NSInteger)tag pressed:(BOOL)pressed emit:(BOOL)emit;
- (void)setMenuPressed:(BOOL)pressed;
- (void)updateThumbIndex:(NSInteger)index forPoint:(CGPoint)point emit:(BOOL)emit;
- (void)updateTouchpadIndex:(NSInteger)index
                 withSamples:(NSArray<UITouch*>*)samples
                        emit:(BOOL)emit;
- (void)resetThumbIndex:(NSInteger)index emit:(BOOL)emit;
- (void)scheduleTouchpadAutoResetForIndex:(NSInteger)index;
- (void)emitThumbClickForIndex:(NSInteger)index;
- (NSInteger)buttonTagForTouchPoint:(CGPoint)point;
- (NSInteger)thumbIndexForTouchPoint:(CGPoint)point;
- (BOOL)isPointInsideMenu:(CGPoint)point;
- (void)emitStateChanged;
- (void)setDigitalButton:(uint16_t)mask pressed:(BOOL)pressed;
- (BOOL)isButtonPressed:(NSInteger)tag;
- (CGRect)frameForControlIdentifier:(NSInteger)controlIdentifier;
- (NSInteger)controlIdentifierForTouchPoint:(CGPoint)point;
- (void)updateEditingHighlight;
- (void)notifyLayoutEditingSelectionChanged;
- (void)notifyLayoutEditingDidChange;
- (void)queueLayoutEditingRefresh;
- (void)rotateSelectedControl:(UIRotationGestureRecognizer*)recognizer;
- (nullable UIColor*)storedColorForControlIdentifier:(NSInteger)controlIdentifier;
- (BOOL)applySnapForSelectedControlWithShortSide:(CGFloat)short_side;
- (CGRect)applyCustomizationToDefaultFrame:(CGRect)default_frame
                          controlIdentifier:(NSInteger)controlIdentifier
                                   minScale:(CGFloat)min_scale
                                   maxScale:(CGFloat)max_scale
                                   shortSide:(CGFloat)short_side
                                  safeInsets:(UIEdgeInsets)insets;
@end

@implementation XeniaTouchControlsView
@synthesize stateDidChangeHandler = stateDidChangeHandler_;
@synthesize menuButtonTappedHandler = menuButtonTappedHandler_;
@synthesize layoutEditingEnabled = layoutEditingEnabled_;
@synthesize layoutEditingSelectionDidChangeHandler = layoutEditingSelectionDidChangeHandler_;
@synthesize layoutEditingDidChangeHandler = layoutEditingDidChangeHandler_;

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.backgroundColor = [UIColor clearColor];
    self.opaque = NO;
    self.multipleTouchEnabled = YES;
    self.exclusiveTouch = NO;
    self.layer.drawsAsynchronously = YES;
    self.layer.shouldRasterize = NO;

    memset(button_frames_, 0, sizeof(button_frames_));
    memset(&menu_frame_, 0, sizeof(menu_frame_));
    memset(thumb_frames_, 0, sizeof(thumb_frames_));
    memset(button_pressed_, 0, sizeof(button_pressed_));
    memset(button_touches_, 0, sizeof(button_touches_));
    memset(thumb_touches_, 0, sizeof(thumb_touches_));
    memset(thumb_touch_start_points_, 0, sizeof(thumb_touch_start_points_));
    memset(thumb_touch_last_points_, 0, sizeof(thumb_touch_last_points_));
    memset(thumb_touch_dragged_, 0, sizeof(thumb_touch_dragged_));
    memset(thumb_touchpad_activity_tokens_, 0, sizeof(thumb_touchpad_activity_tokens_));
    memset(button_image_sizes_, 0, sizeof(button_image_sizes_));
    memset(button_images_, 0, sizeof(button_images_));
    memset(menu_images_, 0, sizeof(menu_images_));
    memset(thumb_vectors_, 0, sizeof(thumb_vectors_));
    memset(button_titles_, 0, sizeof(button_titles_));
    memset(button_colors_, 0, sizeof(button_colors_));
    memset(button_hidden_, 0, sizeof(button_hidden_));
    memset(button_offset_units_, 0, sizeof(button_offset_units_));
    memset(thumb_titles_, 0, sizeof(thumb_titles_));
    memset(thumb_colors_, 0, sizeof(thumb_colors_));
    memset(thumb_hidden_, 0, sizeof(thumb_hidden_));
    memset(thumb_offset_units_, 0, sizeof(thumb_offset_units_));
    memset(button_rotations_, 0, sizeof(button_rotations_));
    memset(thumb_rotations_, 0, sizeof(thumb_rotations_));
    menu_touch_ = nil;
    menu_pressed_ = NO;
    layout_edit_touch_ = nil;
    layout_edit_last_point_ = CGPointZero;
    layout_edit_touch_start_point_ = CGPointZero;
    layout_edit_touch_start_control_identifier_ = -1;
    layout_edit_previous_selected_control_identifier_ = -1;
    layout_edit_touch_moved_ = NO;
    menu_image_size_ = CGSizeZero;
    memset(knob_image_sizes_, 0, sizeof(knob_image_sizes_));
    memset(thumb_base_image_sizes_, 0, sizeof(thumb_base_image_sizes_));
    gameplay_controls_image_size_ = CGSizeZero;
    memset(knob_images_, 0, sizeof(knob_images_));
    memset(thumb_base_images_, 0, sizeof(thumb_base_images_));
    gameplay_controls_image_ = nil;
    for (NSInteger tag = 1; tag <= 14; ++tag) {
      button_scales_[tag] = 1.0f;
      button_shapes_[tag] = XeniaDefaultShapeForControlIdentifier(tag);
    }
    thumb_scales_[0] = 1.0f;
    thumb_scales_[1] = 1.0f;
    thumb_shapes_[0] = XeniaDefaultShapeForControlIdentifier(kXeniaTouchControlIdentifierLeftStick);
    thumb_shapes_[1] = XeniaDefaultShapeForControlIdentifier(kXeniaTouchControlIdentifierRightStick);
    thumb_input_modes_[0] = XeniaTouchStickInputModeJoystick;
    thumb_input_modes_[1] = XeniaTouchStickInputModeJoystick;
    menu_offset_units_ = CGPointZero;
    menu_scale_ = 1.0f;
    menu_rotation_ = 0.0f;
    menu_shape_ = XeniaDefaultShapeForControlIdentifier(kXeniaTouchControlIdentifierMenu);
    menu_hidden_ = NO;
    menu_title_ = nil;
    menu_color_ = nil;
    layoutEditingEnabled_ = NO;
    selectedControlIdentifier_ = -1;
    layout_rotation_gesture_ =
        [[UIRotationGestureRecognizer alloc] initWithTarget:self action:@selector(rotateSelectedControl:)];
    [layout_rotation_gesture_ setEnabled:NO];
    [self addGestureRecognizer:layout_rotation_gesture_];

    for (NSInteger index = 0; index < 2; ++index) {
      thumb_base_layers_[index] = [[CALayer alloc] init];
      XeniaPrepareLayer(thumb_base_layers_[index]);
      [self.layer addSublayer:thumb_base_layers_[index]];
    }
    for (NSInteger tag = 1; tag <= 14; ++tag) {
      button_layers_[tag] = [[CALayer alloc] init];
      XeniaPrepareLayer(button_layers_[tag]);
      [self.layer addSublayer:button_layers_[tag]];
    }
    menu_layer_ = [[CALayer alloc] init];
    XeniaPrepareLayer(menu_layer_);
    [self.layer addSublayer:menu_layer_];
    gameplay_controls_layer_ = [[CALayer alloc] init];
    XeniaPrepareLayer(gameplay_controls_layer_);
    [self.layer addSublayer:gameplay_controls_layer_];
    for (NSInteger index = 0; index < 2; ++index) {
      thumb_layers_[index] = [[CALayer alloc] init];
      XeniaPrepareLayer(thumb_layers_[index]);
      [self.layer addSublayer:thumb_layers_[index]];
    }
  }
  return self;
}

- (void)dealloc {
  [stateDidChangeHandler_ release];
  [menuButtonTappedHandler_ release];
  [layoutEditingSelectionDidChangeHandler_ release];
  [layoutEditingDidChangeHandler_ release];
  for (NSInteger tag = 1; tag <= 14; ++tag) {
    [button_images_[tag][0] release];
    [button_images_[tag][1] release];
    [button_titles_[tag] release];
    [button_colors_[tag] release];
    [button_layers_[tag] release];
  }
  [menu_images_[0] release];
  [menu_images_[1] release];
  [menu_title_ release];
  [menu_color_ release];
  [knob_images_[0] release];
  [knob_images_[1] release];
  [thumb_base_images_[0] release];
  [thumb_base_images_[1] release];
  [gameplay_controls_image_ release];
  [thumb_titles_[0] release];
  [thumb_titles_[1] release];
  [thumb_colors_[0] release];
  [thumb_colors_[1] release];
  [layout_rotation_gesture_ release];
  [menu_layer_ release];
  [gameplay_controls_layer_ release];
  [thumb_base_layers_[0] release];
  [thumb_base_layers_[1] release];
  [thumb_layers_[0] release];
  [thumb_layers_[1] release];
  [super dealloc];
}

- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent*)event {
  (void)event;
  if (layoutEditingEnabled_) {
    return YES;
  }
  if ([self thumbIndexForTouchPoint:point] >= 0) {
    return YES;
  }
  if ([self isPointInsideMenu:point]) {
    return YES;
  }
  return [self buttonTagForTouchPoint:point] >= 0;
}

- (void)rebuildAssetsIfNeededForButtonTag:(NSInteger)tag size:(CGSize)size {
  if (CGSizeEqualToSize(size, button_image_sizes_[tag])) {
    return;
  }
  [button_images_[tag][0] release];
  [button_images_[tag][1] release];
  NSString* title = button_titles_[tag] ?: XeniaButtonTitle(tag);
  UIColor* base_color = button_colors_[tag];
  UIColor* normal_color = base_color ?: XeniaDefaultColorForControlIdentifier(tag, NO);
  UIColor* pressed_color = base_color ? XeniaDarkenedColor(base_color, 0.18f)
                                      : XeniaDefaultColorForControlIdentifier(tag, YES);
  XeniaTouchControlShape shape = static_cast<XeniaTouchControlShape>(button_shapes_[tag]);
  button_images_[tag][0] =
      [XeniaCreateControlImage(size, tag, NO, title, normal_color, shape) retain];
  button_images_[tag][1] =
      [XeniaCreateControlImage(size, tag, YES, title, pressed_color, shape) retain];
  button_image_sizes_[tag] = size;
}

- (void)rebuildMenuAssetsIfNeeded:(CGSize)size {
  if (CGSizeEqualToSize(size, menu_image_size_)) {
    return;
  }
  [menu_images_[0] release];
  [menu_images_[1] release];
  NSString* title = menu_title_ ?: XeniaDefaultTitleForControlIdentifier(kXeniaTouchControlIdentifierMenu);
  UIColor* normal_color = menu_color_ ?: XeniaDefaultColorForControlIdentifier(kXeniaTouchControlIdentifierMenu, NO);
  UIColor* pressed_color = menu_color_ ? XeniaDarkenedColor(menu_color_, 0.18f)
                                       : XeniaDefaultColorForControlIdentifier(
                                             kXeniaTouchControlIdentifierMenu, YES);
  XeniaTouchControlShape shape = static_cast<XeniaTouchControlShape>(menu_shape_);
  menu_images_[0] = [XeniaCreateControlImage(
      size, kXeniaTouchControlIdentifierMenu, NO, title, normal_color, shape) retain];
  menu_images_[1] = [XeniaCreateControlImage(
      size, kXeniaTouchControlIdentifierMenu, YES, title, pressed_color, shape) retain];
  menu_image_size_ = size;
}

- (void)rebuildKnobAssetIfNeededForIndex:(NSInteger)index size:(CGSize)size {
  if (index < 0 || index > 1 || CGSizeEqualToSize(size, knob_image_sizes_[index])) {
    return;
  }
  [knob_images_[index] release];
  NSInteger control_identifier =
      index == 0 ? kXeniaTouchControlIdentifierLeftStick : kXeniaTouchControlIdentifierRightStick;
  if (thumb_input_modes_[index] == XeniaTouchStickInputModeTouchpad) {
    knob_images_[index] = nil;
    knob_image_sizes_[index] = size;
    return;
  }
  knob_images_[index] = [XeniaCreateControlImage(
      size, control_identifier, NO,
      thumb_titles_[index] ?: XeniaDefaultTitleForControlIdentifier(control_identifier),
      thumb_colors_[index] ?: XeniaDefaultColorForControlIdentifier(control_identifier, NO),
      XeniaTouchControlShapeCircle) retain];
  knob_image_sizes_[index] = size;
}

- (void)rebuildThumbBaseAssetIfNeededForIndex:(NSInteger)index size:(CGSize)size {
  if (index < 0 || index > 1 || CGSizeEqualToSize(size, thumb_base_image_sizes_[index])) {
    return;
  }
  [thumb_base_images_[index] release];
  thumb_base_images_[index] = nil;
  thumb_base_image_sizes_[index] = size;
}

- (void)invalidateButtonAssetsForTag:(NSInteger)tag {
  [button_images_[tag][0] release];
  button_images_[tag][0] = nil;
  [button_images_[tag][1] release];
  button_images_[tag][1] = nil;
  button_image_sizes_[tag] = CGSizeZero;
}

- (void)invalidateMenuAssets {
  [menu_images_[0] release];
  menu_images_[0] = nil;
  [menu_images_[1] release];
  menu_images_[1] = nil;
  menu_image_size_ = CGSizeZero;
}

- (void)invalidateKnobAssets {
  [knob_images_[0] release];
  knob_images_[0] = nil;
  [knob_images_[1] release];
  knob_images_[1] = nil;
  [thumb_base_images_[0] release];
  thumb_base_images_[0] = nil;
  [thumb_base_images_[1] release];
  thumb_base_images_[1] = nil;
  knob_image_sizes_[0] = CGSizeZero;
  knob_image_sizes_[1] = CGSizeZero;
  thumb_base_image_sizes_[0] = CGSizeZero;
  thumb_base_image_sizes_[1] = CGSizeZero;
}

- (void)invalidateGameplayControlsComposite {
  [gameplay_controls_image_ release];
  gameplay_controls_image_ = nil;
  gameplay_controls_image_size_ = CGSizeZero;
}

- (void)rebuildGameplayControlsCompositeIfNeeded {
  CGSize size = self.bounds.size;
  if (CGSizeEqualToSize(size, CGSizeZero)) {
    [self invalidateGameplayControlsComposite];
    return;
  }
  if (gameplay_controls_image_ && CGSizeEqualToSize(gameplay_controls_image_size_, size)) {
    return;
  }
  [gameplay_controls_image_ release];
  gameplay_controls_image_ = [XeniaRenderImage(size, ^(CGRect __unused rect) {
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    for (NSInteger tag = 1; tag <= 14; ++tag) {
      if (button_hidden_[tag]) {
        continue;
      }
      UIImage* image = button_images_[tag][[self isButtonPressed:tag] ? 1 : 0];
      XeniaDrawImageInRect(ctx, image, button_frames_[tag], button_rotations_[tag]);
    }
    if (!menu_hidden_) {
      UIImage* menu_image = menu_images_[menu_pressed_ ? 1 : 0];
      XeniaDrawImageInRect(ctx, menu_image, menu_frame_, menu_rotation_);
    }
  }) retain];
  gameplay_controls_image_size_ = size;
}

- (void)updateGameplayControlsLayer {
  [self rebuildGameplayControlsCompositeIfNeeded];
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  gameplay_controls_layer_.bounds = self.bounds;
  gameplay_controls_layer_.position =
      CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
  gameplay_controls_layer_.hidden = self.hidden || layoutEditingEnabled_ || gameplay_controls_image_ == nil;
  gameplay_controls_layer_.contents = (id)gameplay_controls_image_.CGImage;
  gameplay_controls_layer_.affineTransform = CGAffineTransformIdentity;
  [CATransaction commit];
}

- (void)updateButtonLayerForTag:(NSInteger)tag {
  CALayer* layer = button_layers_[tag];
  if (!layoutEditingEnabled_) {
    layer.hidden = YES;
    return;
  }
  UIImage* image = button_images_[tag][[self isButtonPressed:tag] ? 1 : 0];
  CGRect frame = button_frames_[tag];
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  layer.bounds = CGRectMake(0.0f, 0.0f, CGRectGetWidth(frame), CGRectGetHeight(frame));
  layer.position = CGPointMake(CGRectGetMidX(frame), CGRectGetMidY(frame));
  layer.hidden = button_hidden_[tag] || CGRectIsEmpty(frame) || image == nil || self.hidden;
  layer.contents = (id)image.CGImage;
  layer.affineTransform = CGAffineTransformMakeRotation(button_rotations_[tag]);
  [CATransaction commit];
}

- (void)updateMenuLayer {
  if (!layoutEditingEnabled_) {
    menu_layer_.hidden = YES;
    return;
  }
  UIImage* image = menu_images_[menu_pressed_ ? 1 : 0];
  CGRect frame = menu_frame_;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  menu_layer_.bounds = CGRectMake(0.0f, 0.0f, CGRectGetWidth(frame), CGRectGetHeight(frame));
  menu_layer_.position = CGPointMake(CGRectGetMidX(frame), CGRectGetMidY(frame));
  menu_layer_.hidden = menu_hidden_ || CGRectIsEmpty(frame) || image == nil || self.hidden;
  menu_layer_.contents = (id)image.CGImage;
  menu_layer_.affineTransform = CGAffineTransformMakeRotation(menu_rotation_);
  [CATransaction commit];
}

- (CGRect)thumbKnobRectForIndex:(NSInteger)index {
  return XeniaThumbKnobRect(thumb_frames_[index], thumb_vectors_[index],
                            static_cast<XeniaTouchControlShape>(thumb_shapes_[index]),
                            static_cast<XeniaTouchStickInputMode>(thumb_input_modes_[index]));
}

- (CGRect)thumbHitRectForIndex:(NSInteger)index {
  if (thumb_input_modes_[index] == XeniaTouchStickInputModeTouchpad) {
    return CGRectMake(CGRectGetMidX(self.bounds), 0.0f,
                      CGRectGetWidth(self.bounds) - CGRectGetMidX(self.bounds),
                      CGRectGetHeight(self.bounds));
  }
  return [self thumbKnobRectForIndex:index];
}

- (void)updateThumbBaseLayerForIndex:(NSInteger)index {
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  thumb_base_layers_[index].bounds = CGRectZero;
  thumb_base_layers_[index].position = CGPointZero;
  thumb_base_layers_[index].hidden = YES;
  thumb_base_layers_[index].contents = nil;
  thumb_base_layers_[index].affineTransform = CGAffineTransformMakeRotation(thumb_rotations_[index]);
  [CATransaction commit];
}

- (void)updateThumbLayerForIndex:(NSInteger)index {
  CGRect frame = [self thumbKnobRectForIndex:index];
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  UIImage* knob_image = knob_images_[index];
  BOOL should_show_knob = !self.hidden && knob_image != nil &&
                          !thumb_hidden_[index] &&
                          thumb_input_modes_[index] != XeniaTouchStickInputModeTouchpad;
  thumb_layers_[index].bounds =
      should_show_knob ? CGRectMake(0.0f, 0.0f, CGRectGetWidth(frame), CGRectGetHeight(frame)) : CGRectZero;
  thumb_layers_[index].position =
      should_show_knob ? CGPointMake(CGRectGetMidX(frame), CGRectGetMidY(frame)) : CGPointZero;
  thumb_layers_[index].hidden = !should_show_knob;
  thumb_layers_[index].contents = should_show_knob ? (id)knob_image.CGImage : nil;
  thumb_layers_[index].affineTransform = CGAffineTransformMakeRotation(thumb_rotations_[index]);
  [CATransaction commit];
}

- (void)setHidden:(BOOL)hidden {
  BOOL changed = self.hidden != hidden;
  [super setHidden:hidden];
  if (!changed) {
    return;
  }
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  for (NSInteger tag = 1; tag <= 14; ++tag) {
    button_layers_[tag].hidden = hidden || !layoutEditingEnabled_;
  }
  menu_layer_.hidden = hidden || !layoutEditingEnabled_;
  gameplay_controls_layer_.hidden = hidden || layoutEditingEnabled_ || gameplay_controls_image_ == nil;
  [CATransaction commit];
  [self updateThumbBaseLayerForIndex:0];
  [self updateThumbBaseLayerForIndex:1];
  [self updateThumbLayerForIndex:0];
  [self updateThumbLayerForIndex:1];
}

- (void)layoutSubviews {
  [super layoutSubviews];

  UIEdgeInsets insets = self.safeAreaInsets;
  CGFloat width = CGRectGetWidth(self.bounds);
  CGFloat height = CGRectGetHeight(self.bounds);
  CGFloat short_side = MIN(width, height);
  BOOL is_pad = UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad;
  XeniaTouchDefaultLayout layout = XeniaBuildDefaultTouchLayout(width, height, insets, is_pad);
  CGFloat reference_width = is_pad ? 1194.0f : 932.0f;
  CGFloat reference_height = is_pad ? 834.0f : 430.0f;
  UIEdgeInsets reference_insets =
      is_pad ? UIEdgeInsetsZero : UIEdgeInsetsMake(0.0f, 59.0f, 0.0f, 59.0f);
  XeniaTouchDefaultLayout reference_layout =
      XeniaBuildDefaultTouchLayout(reference_width, reference_height,
                                   reference_insets, is_pad);
  if (!is_pad) {
    CGFloat left_shoulder_shift =
        CGRectGetMinX(reference_layout.button_frames[7]) -
        CGRectGetMinX(reference_layout.button_frames[1]);
    reference_layout.button_frames[1].origin.x += left_shoulder_shift;
    reference_layout.button_frames[2].origin.x += left_shoulder_shift;
    CGFloat right_shoulder_shift =
        CGRectGetMaxX(reference_layout.button_frames[12]) -
        CGRectGetMaxX(reference_layout.button_frames[4]);
    reference_layout.button_frames[3].origin.x += right_shoulder_shift;
    reference_layout.button_frames[4].origin.x += right_shoulder_shift;
  }
  CGFloat scale_x = width / reference_width;
  CGFloat scale_y = height / reference_height;
  auto scale_rect = ^CGRect(CGRect rect) {
    return CGRectMake(rect.origin.x * scale_x, rect.origin.y * scale_y,
                      rect.size.width * scale_x, rect.size.height * scale_y);
  };
  for (NSInteger tag = 1; tag <= 14; ++tag) {
    layout.button_frames[tag] = scale_rect(reference_layout.button_frames[tag]);
  }
  layout.menu_frame = scale_rect(reference_layout.menu_frame);
  layout.thumb_frames[0] = scale_rect(reference_layout.thumb_frames[0]);
  layout.thumb_frames[1] = scale_rect(reference_layout.thumb_frames[1]);
  if (is_pad) {
    CGFloat gameplay_height = MIN(height, width * (9.0f / 16.0f));
    CGFloat gameplay_top = floorf((height - gameplay_height) * 0.5f);
    CGFloat gameplay_bottom = gameplay_top + gameplay_height;
    CGFloat gameplay_cluster_bottom =
        MAX(CGRectGetMaxY(layout.button_frames[6]), CGRectGetMaxY(layout.button_frames[11]));
    CGFloat gameplay_cluster_shift = gameplay_bottom - gameplay_cluster_bottom;
    for (NSInteger tag = 5; tag <= 12; ++tag) {
      layout.button_frames[tag].origin.y += gameplay_cluster_shift;
    }
    layout.thumb_frames[0].origin.y += gameplay_cluster_shift;
    layout.thumb_frames[1].origin.y += gameplay_cluster_shift;
  }

  for (NSInteger tag = 1; tag <= 14; ++tag) {
    button_frames_[tag] = layout.button_frames[tag];
  }
  menu_frame_ = layout.menu_frame;
  thumb_frames_[0] = layout.thumb_frames[0];
  thumb_frames_[1] = layout.thumb_frames[1];

  UIEdgeInsets clamp_insets = UIEdgeInsetsZero;
  for (NSInteger tag = 1; tag <= 14; ++tag) {
    button_frames_[tag] = [self applyCustomizationToDefaultFrame:button_frames_[tag]
                                                controlIdentifier:tag
                                                         minScale:0.6f
                                                         maxScale:5.0f
                                                        shortSide:short_side
                                                       safeInsets:clamp_insets];
  }
  menu_frame_ = [self applyCustomizationToDefaultFrame:menu_frame_
                                     controlIdentifier:kXeniaTouchControlIdentifierMenu
                                              minScale:0.7f
                                              maxScale:5.0f
                                             shortSide:short_side
                                            safeInsets:clamp_insets];
  thumb_frames_[0] = [self applyCustomizationToDefaultFrame:thumb_frames_[0]
                                           controlIdentifier:kXeniaTouchControlIdentifierLeftStick
                                                    minScale:0.6f
                                                    maxScale:5.0f
                                                   shortSide:short_side
                                                  safeInsets:clamp_insets];
  thumb_frames_[1] = [self applyCustomizationToDefaultFrame:thumb_frames_[1]
                                           controlIdentifier:kXeniaTouchControlIdentifierRightStick
                                                    minScale:0.6f
                                                    maxScale:5.0f
                                                   shortSide:short_side
                                                  safeInsets:clamp_insets];

  for (NSInteger tag = 1; tag <= 14; ++tag) {
    [self rebuildAssetsIfNeededForButtonTag:tag size:button_frames_[tag].size];
    [self updateButtonLayerForTag:tag];
  }
  [self rebuildMenuAssetsIfNeeded:menu_frame_.size];
  [self updateMenuLayer];
  [self rebuildThumbBaseAssetIfNeededForIndex:0 size:thumb_frames_[0].size];
  [self rebuildKnobAssetIfNeededForIndex:0
                                    size:XeniaThumbKnobSize(
                                             thumb_frames_[0],
                                             static_cast<XeniaTouchControlShape>(thumb_shapes_[0]),
                                             static_cast<XeniaTouchStickInputMode>(thumb_input_modes_[0]))];
  [self rebuildThumbBaseAssetIfNeededForIndex:1 size:thumb_frames_[1].size];
  [self rebuildKnobAssetIfNeededForIndex:1
                                    size:XeniaThumbKnobSize(
                                             thumb_frames_[1],
                                             static_cast<XeniaTouchControlShape>(thumb_shapes_[1]),
                                             static_cast<XeniaTouchStickInputMode>(thumb_input_modes_[1]))];
  [self invalidateGameplayControlsComposite];
  [self updateGameplayControlsLayer];
  [self updateThumbBaseLayerForIndex:0];
  [self updateThumbBaseLayerForIndex:1];
  [self updateThumbLayerForIndex:0];
  [self updateThumbLayerForIndex:1];
  [self updateEditingHighlight];
}

- (xe::hid::X_INPUT_STATE)currentControllerState {
  return state_;
}

- (void)resetControllerState {
  state_ = xe::hid::X_INPUT_STATE{};
  memset(button_pressed_, 0, sizeof(button_pressed_));
  memset(button_touches_, 0, sizeof(button_touches_));
  memset(thumb_touches_, 0, sizeof(thumb_touches_));
  memset(thumb_touch_start_points_, 0, sizeof(thumb_touch_start_points_));
  memset(thumb_touch_last_points_, 0, sizeof(thumb_touch_last_points_));
  memset(thumb_touch_dragged_, 0, sizeof(thumb_touch_dragged_));
  thumb_vectors_[0] = ThumbVector();
  thumb_vectors_[1] = ThumbVector();
  menu_pressed_ = NO;
  menu_touch_ = nil;
  for (NSInteger tag = 1; tag <= 14; ++tag) {
    [self updateButtonLayerForTag:tag];
  }
  [self updateMenuLayer];
  [self invalidateGameplayControlsComposite];
  [self updateGameplayControlsLayer];
  [self updateThumbBaseLayerForIndex:0];
  [self updateThumbBaseLayerForIndex:1];
  [self updateThumbLayerForIndex:0];
  [self updateThumbLayerForIndex:1];
  [self emitStateChanged];
}

- (void)emitStateChanged {
  state_.packet_number += 1;
  if (stateDidChangeHandler_) {
    stateDidChangeHandler_();
  }
}

- (NSInteger)selectedControlIdentifier { return selectedControlIdentifier_; }

- (void)setLayoutEditingEnabled:(BOOL)layoutEditingEnabled {
  if (layoutEditingEnabled_ == layoutEditingEnabled) {
    return;
  }
  layoutEditingEnabled_ = layoutEditingEnabled;
  [layout_rotation_gesture_ setEnabled:layoutEditingEnabled];
  layout_edit_touch_ = nil;
  layout_edit_last_point_ = CGPointZero;
  layout_edit_touch_start_point_ = CGPointZero;
  layout_edit_touch_start_control_identifier_ = -1;
  layout_edit_previous_selected_control_identifier_ = -1;
  layout_edit_touch_moved_ = NO;
  if (layoutEditingEnabled_) {
    [self resetControllerState];
  } else {
    selectedControlIdentifier_ = -1;
    [self notifyLayoutEditingSelectionChanged];
  }
  [self invalidateGameplayControlsComposite];
  [self setNeedsLayout];
  [self updateEditingHighlight];
}

- (CGFloat)scaleForControlIdentifier:(NSInteger)controlIdentifier {
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    return button_scales_[controlIdentifier];
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    return menu_scale_;
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick) {
    return thumb_scales_[0];
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    return thumb_scales_[1];
  }
  return 1.0f;
}

- (NSString*)titleForControlIdentifier:(NSInteger)controlIdentifier {
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    return button_titles_[controlIdentifier] != nil ? button_titles_[controlIdentifier]
                                                    : XeniaDefaultTitleForControlIdentifier(controlIdentifier);
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    return menu_title_ != nil ? menu_title_
                              : XeniaDefaultTitleForControlIdentifier(controlIdentifier);
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick) {
    return thumb_titles_[0] != nil ? thumb_titles_[0]
                                   : XeniaDefaultTitleForControlIdentifier(controlIdentifier);
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    return thumb_titles_[1] != nil ? thumb_titles_[1]
                                   : XeniaDefaultTitleForControlIdentifier(controlIdentifier);
  }
  return @"";
}

- (void)setScale:(CGFloat)scale forControlIdentifier:(NSInteger)controlIdentifier {
  CGFloat clamped = XeniaClamp(scale, 0.6f, 5.0f);
  BOOL changed = NO;
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    if (fabs(button_scales_[controlIdentifier] - clamped) <= 0.0005f) {
      return;
    }
    button_scales_[controlIdentifier] = clamped;
    changed = YES;
  } else if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    if (fabs(menu_scale_ - clamped) <= 0.0005f) {
      return;
    }
    menu_scale_ = clamped;
    changed = YES;
  } else if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick) {
    if (fabs(thumb_scales_[0] - clamped) <= 0.0005f) {
      return;
    }
    thumb_scales_[0] = clamped;
    changed = YES;
  } else if (controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    if (fabs(thumb_scales_[1] - clamped) <= 0.0005f) {
      return;
    }
    thumb_scales_[1] = clamped;
    changed = YES;
  } else {
    return;
  }
  if (changed) {
    [self queueLayoutEditingRefresh];
  }
}

- (NSInteger)shapeForControlIdentifier:(NSInteger)controlIdentifier {
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    return button_shapes_[controlIdentifier];
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    return menu_shape_;
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick) {
    return thumb_shapes_[0];
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    return thumb_shapes_[1];
  }
  return XeniaTouchControlShapeCircle;
}

- (NSInteger)inputModeForControlIdentifier:(NSInteger)controlIdentifier {
  if (controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    return thumb_input_modes_[1];
  }
  return XeniaTouchStickInputModeJoystick;
}

- (nullable UIColor*)storedColorForControlIdentifier:(NSInteger)controlIdentifier {
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    return button_colors_[controlIdentifier];
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    return menu_color_;
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick) {
    return thumb_colors_[0];
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    return thumb_colors_[1];
  }
  return nil;
}

- (nullable UIColor*)colorForControlIdentifier:(NSInteger)controlIdentifier {
  return [self storedColorForControlIdentifier:controlIdentifier] ?:
         XeniaDefaultColorForControlIdentifier(controlIdentifier, NO);
}

- (BOOL)isHiddenForControlIdentifier:(NSInteger)controlIdentifier {
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    return button_hidden_[controlIdentifier];
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    return menu_hidden_;
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick) {
    return thumb_hidden_[0];
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    return thumb_hidden_[1];
  }
  return NO;
}

- (void)setHidden:(BOOL)hidden forControlIdentifier:(NSInteger)controlIdentifier {
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    if (button_hidden_[controlIdentifier] == hidden) {
      return;
    }
    button_hidden_[controlIdentifier] = hidden;
    [self invalidateButtonAssetsForTag:controlIdentifier];
    [self invalidateGameplayControlsComposite];
  } else if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    if (menu_hidden_ == hidden) {
      return;
    }
    menu_hidden_ = hidden;
    [self invalidateMenuAssets];
    [self invalidateGameplayControlsComposite];
  } else if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick ||
             controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    NSInteger index = controlIdentifier == kXeniaTouchControlIdentifierLeftStick ? 0 : 1;
    if (thumb_hidden_[index] == hidden) {
      return;
    }
    thumb_hidden_[index] = hidden;
    [self invalidateKnobAssets];
  } else {
    return;
  }
  [self setNeedsLayout];
  [self queueLayoutEditingRefresh];
}

- (void)setTitle:(NSString*)title forControlIdentifier:(NSInteger)controlIdentifier {
  NSString* trimmed =
      [[title ?: @"" stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]] copy];
  NSString* normalized = [trimmed copy];
  [trimmed release];
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    if ([button_titles_[controlIdentifier] isEqualToString:normalized]) {
      [normalized release];
      return;
    }
    [button_titles_[controlIdentifier] release];
    button_titles_[controlIdentifier] = normalized;
    [self invalidateButtonAssetsForTag:controlIdentifier];
    [self invalidateGameplayControlsComposite];
    [self setNeedsLayout];
  } else if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    if ([menu_title_ isEqualToString:normalized]) {
      [normalized release];
      return;
    }
    [menu_title_ release];
    menu_title_ = normalized;
    [self invalidateMenuAssets];
    [self invalidateGameplayControlsComposite];
    [self setNeedsLayout];
  } else if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick ||
             controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    NSInteger index = controlIdentifier == kXeniaTouchControlIdentifierLeftStick ? 0 : 1;
    if ([thumb_titles_[index] isEqualToString:normalized]) {
      [normalized release];
      return;
    }
    [thumb_titles_[index] release];
    thumb_titles_[index] = normalized;
    [self invalidateKnobAssets];
    [self setNeedsLayout];
  } else {
    [normalized release];
    return;
  }
  [self queueLayoutEditingRefresh];
}

- (void)setColor:(UIColor*)color forControlIdentifier:(NSInteger)controlIdentifier {
  UIColor* existing = [self storedColorForControlIdentifier:controlIdentifier];
  if ((existing == nil && color == nil) || [existing isEqual:color]) {
    return;
  }
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    [button_colors_[controlIdentifier] release];
    button_colors_[controlIdentifier] = [color retain];
    [self invalidateButtonAssetsForTag:controlIdentifier];
    [self invalidateGameplayControlsComposite];
    [self setNeedsLayout];
  } else if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    [menu_color_ release];
    menu_color_ = [color retain];
    [self invalidateMenuAssets];
    [self invalidateGameplayControlsComposite];
    [self setNeedsLayout];
  } else if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick ||
             controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    NSInteger index = controlIdentifier == kXeniaTouchControlIdentifierLeftStick ? 0 : 1;
    [thumb_colors_[index] release];
    thumb_colors_[index] = [color retain];
    [self invalidateKnobAssets];
    [self setNeedsLayout];
  } else {
    return;
  }
  [self queueLayoutEditingRefresh];
}

- (void)setShape:(NSInteger)shape forControlIdentifier:(NSInteger)controlIdentifier {
  NSInteger normalized_shape = shape;
  if (normalized_shape < XeniaTouchControlShapeCircle ||
      normalized_shape > XeniaTouchControlShapeRoundedSquare) {
    normalized_shape = XeniaTouchControlShapeCircle;
  }
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    if (button_shapes_[controlIdentifier] == normalized_shape) {
      return;
    }
    button_shapes_[controlIdentifier] = normalized_shape;
    [self invalidateButtonAssetsForTag:controlIdentifier];
    [self invalidateGameplayControlsComposite];
    [self setNeedsLayout];
  } else if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    if (menu_shape_ == normalized_shape) {
      return;
    }
    menu_shape_ = normalized_shape;
    [self invalidateMenuAssets];
    [self invalidateGameplayControlsComposite];
    [self setNeedsLayout];
  } else if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick ||
             controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    NSInteger index = controlIdentifier == kXeniaTouchControlIdentifierLeftStick ? 0 : 1;
    if (thumb_shapes_[index] == normalized_shape) {
      return;
    }
    thumb_shapes_[index] = normalized_shape;
    [self invalidateKnobAssets];
    [self setNeedsLayout];
  } else {
    return;
  }
  [self queueLayoutEditingRefresh];
}

- (void)setInputMode:(NSInteger)inputMode forControlIdentifier:(NSInteger)controlIdentifier {
  if (controlIdentifier != kXeniaTouchControlIdentifierRightStick) {
    return;
  }
  NSInteger normalized_mode = inputMode == XeniaTouchStickInputModeTouchpad
                                  ? XeniaTouchStickInputModeTouchpad
                                  : XeniaTouchStickInputModeJoystick;
  NSInteger index = 1;
  if (thumb_input_modes_[index] == normalized_mode) {
    return;
  }
  thumb_input_modes_[index] = normalized_mode;
  thumb_vectors_[index] = ThumbVector();
  [self invalidateKnobAssets];
  [self queueLayoutEditingRefresh];
}

- (NSDictionary*)layoutConfiguration {
  NSMutableDictionary* button_configs = [NSMutableDictionary dictionary];
  for (NSInteger tag = 1; tag <= 14; ++tag) {
    NSMutableDictionary* button_config = [NSMutableDictionary dictionaryWithDictionary:@{
      @"x" : XeniaNumberOrNil(button_offset_units_[tag].x),
      @"y" : XeniaNumberOrNil(button_offset_units_[tag].y),
      @"scale" : XeniaNumberOrNil(button_scales_[tag]),
      @"rotation" : XeniaNumberOrNil(button_rotations_[tag]),
      @"shape" : @(button_shapes_[tag]),
      @"hidden" : @(button_hidden_[tag]),
    }];
    if (button_titles_[tag].length > 0) {
      button_config[@"title"] = button_titles_[tag];
    }
    NSDictionary* color_config = XeniaColorConfiguration(button_colors_[tag]);
    if (color_config) {
      button_config[@"color"] = color_config;
    }
    button_configs[[NSString stringWithFormat:@"%ld", (long)tag]] = button_config;
  }
  NSMutableDictionary* menu_config = [NSMutableDictionary dictionaryWithDictionary:@{
    @"x" : XeniaNumberOrNil(menu_offset_units_.x),
    @"y" : XeniaNumberOrNil(menu_offset_units_.y),
    @"scale" : XeniaNumberOrNil(menu_scale_),
    @"rotation" : XeniaNumberOrNil(menu_rotation_),
    @"shape" : @(menu_shape_),
    @"hidden" : @(menu_hidden_),
  }];
  if (menu_title_.length > 0) {
    menu_config[@"title"] = menu_title_;
  }
  NSDictionary* menu_color_config = XeniaColorConfiguration(menu_color_);
  if (menu_color_config) {
    menu_config[@"color"] = menu_color_config;
  }
  NSMutableDictionary* left_stick = [NSMutableDictionary dictionaryWithDictionary:@{
    @"x" : XeniaNumberOrNil(thumb_offset_units_[0].x),
    @"y" : XeniaNumberOrNil(thumb_offset_units_[0].y),
    @"scale" : XeniaNumberOrNil(thumb_scales_[0]),
    @"rotation" : XeniaNumberOrNil(thumb_rotations_[0]),
    @"shape" : @(thumb_shapes_[0]),
    @"mode" : @(XeniaTouchStickInputModeJoystick),
    @"hidden" : @(thumb_hidden_[0]),
  }];
  if (thumb_titles_[0].length > 0) {
    left_stick[@"title"] = thumb_titles_[0];
  }
  NSDictionary* left_color_config = XeniaColorConfiguration(thumb_colors_[0]);
  if (left_color_config) {
    left_stick[@"color"] = left_color_config;
  }
  NSMutableDictionary* right_stick = [NSMutableDictionary dictionaryWithDictionary:@{
    @"x" : XeniaNumberOrNil(thumb_offset_units_[1].x),
    @"y" : XeniaNumberOrNil(thumb_offset_units_[1].y),
    @"scale" : XeniaNumberOrNil(thumb_scales_[1]),
    @"rotation" : XeniaNumberOrNil(thumb_rotations_[1]),
    @"shape" : @(thumb_shapes_[1]),
    @"mode" : @(thumb_input_modes_[1]),
    @"hidden" : @(thumb_hidden_[1]),
  }];
  if (thumb_titles_[1].length > 0) {
    right_stick[@"title"] = thumb_titles_[1];
  }
  NSDictionary* right_color_config = XeniaColorConfiguration(thumb_colors_[1]);
  if (right_color_config) {
    right_stick[@"color"] = right_color_config;
  }
  return @{
    @"buttons" : button_configs,
    @"menu" : menu_config,
    @"sticks" : @{
      @"left" : left_stick,
      @"right" : right_stick,
    },
  };
}

- (void)applyLayoutConfiguration:(NSDictionary*)configuration {
  [self resetLayoutConfigurationToDefaults];
  if (![configuration isKindOfClass:[NSDictionary class]]) {
    [self setNeedsLayout];
    return;
  }
  NSDictionary* button_configs = [configuration[@"buttons"] isKindOfClass:[NSDictionary class]]
                                     ? configuration[@"buttons"]
                                     : nil;
  for (NSInteger tag = 1; tag <= 14; ++tag) {
    NSDictionary* button_config =
        [button_configs[[NSString stringWithFormat:@"%ld", (long)tag]] isKindOfClass:[NSDictionary class]]
            ? button_configs[[NSString stringWithFormat:@"%ld", (long)tag]]
            : nil;
    if (button_config) {
      button_offset_units_[tag] =
          CGPointMake([button_config[@"x"] doubleValue], [button_config[@"y"] doubleValue]);
      button_scales_[tag] = XeniaClamp([button_config[@"scale"] doubleValue], 0.6f, 5.0f);
      if (button_scales_[tag] <= 0.0f) {
        button_scales_[tag] = 1.0f;
      }
      button_rotations_[tag] = [button_config[@"rotation"] doubleValue];
      NSNumber* shape_number =
          [button_config[@"shape"] isKindOfClass:[NSNumber class]] ? button_config[@"shape"] : nil;
      button_shapes_[tag] = shape_number ? XeniaClamp(shape_number.integerValue,
                                                      XeniaTouchControlShapeCircle,
                                                      XeniaTouchControlShapeRoundedSquare)
                                         : XeniaDefaultShapeForControlIdentifier(tag);
      NSNumber* hidden_number =
          [button_config[@"hidden"] isKindOfClass:[NSNumber class]] ? button_config[@"hidden"] : nil;
      button_hidden_[tag] = hidden_number ? hidden_number.boolValue : NO;
      NSString* title = [button_config[@"title"] isKindOfClass:[NSString class]] ? button_config[@"title"] : nil;
      [button_titles_[tag] release];
      button_titles_[tag] = title.length > 0 ? [title copy] : nil;
      UIColor* color = XeniaColorFromConfiguration(button_config[@"color"]);
      [button_colors_[tag] release];
      button_colors_[tag] = [color retain];
    }
  }
  NSDictionary* menu_config = [configuration[@"menu"] isKindOfClass:[NSDictionary class]]
                                  ? configuration[@"menu"]
                                  : nil;
  if (menu_config) {
    menu_offset_units_ = CGPointMake([menu_config[@"x"] doubleValue], [menu_config[@"y"] doubleValue]);
    menu_scale_ = XeniaClamp([menu_config[@"scale"] doubleValue], 0.6f, 5.0f);
    if (menu_scale_ <= 0.0f) {
      menu_scale_ = 1.0f;
    }
    menu_rotation_ = [menu_config[@"rotation"] doubleValue];
    NSNumber* shape_number =
        [menu_config[@"shape"] isKindOfClass:[NSNumber class]] ? menu_config[@"shape"] : nil;
    menu_shape_ = shape_number ? XeniaClamp(shape_number.integerValue, XeniaTouchControlShapeCircle,
                                            XeniaTouchControlShapeRoundedSquare)
                               : XeniaDefaultShapeForControlIdentifier(kXeniaTouchControlIdentifierMenu);
    NSNumber* hidden_number =
        [menu_config[@"hidden"] isKindOfClass:[NSNumber class]] ? menu_config[@"hidden"] : nil;
    menu_hidden_ = hidden_number ? hidden_number.boolValue : NO;
    NSString* title = [menu_config[@"title"] isKindOfClass:[NSString class]] ? menu_config[@"title"] : nil;
    [menu_title_ release];
    menu_title_ = title.length > 0 ? [title copy] : nil;
    UIColor* color = XeniaColorFromConfiguration(menu_config[@"color"]);
    [menu_color_ release];
    menu_color_ = [color retain];
  }
  NSDictionary* sticks = [configuration[@"sticks"] isKindOfClass:[NSDictionary class]]
                             ? configuration[@"sticks"]
                             : nil;
  NSArray<NSString*>* stick_keys = @[ @"left", @"right" ];
  for (NSInteger index = 0; index < 2; ++index) {
    NSDictionary* stick_config = [sticks[stick_keys[index]] isKindOfClass:[NSDictionary class]]
                                     ? sticks[stick_keys[index]]
                                     : nil;
    if (stick_config) {
      thumb_offset_units_[index] =
          CGPointMake([stick_config[@"x"] doubleValue], [stick_config[@"y"] doubleValue]);
      thumb_scales_[index] = XeniaClamp([stick_config[@"scale"] doubleValue], 0.6f, 5.0f);
      if (thumb_scales_[index] <= 0.0f) {
        thumb_scales_[index] = 1.0f;
      }
      thumb_rotations_[index] = [stick_config[@"rotation"] doubleValue];
      NSNumber* shape_number =
          [stick_config[@"shape"] isKindOfClass:[NSNumber class]] ? stick_config[@"shape"] : nil;
      thumb_shapes_[index] = shape_number ? XeniaClamp(shape_number.integerValue,
                                                       XeniaTouchControlShapeCircle,
                                                       XeniaTouchControlShapeRoundedSquare)
                                          : XeniaDefaultShapeForControlIdentifier(
                                                index == 0 ? kXeniaTouchControlIdentifierLeftStick
                                                           : kXeniaTouchControlIdentifierRightStick);
      NSNumber* hidden_number =
          [stick_config[@"hidden"] isKindOfClass:[NSNumber class]] ? stick_config[@"hidden"] : nil;
      thumb_hidden_[index] = hidden_number ? hidden_number.boolValue : NO;
      NSNumber* mode_number =
          [stick_config[@"mode"] isKindOfClass:[NSNumber class]] ? stick_config[@"mode"] : nil;
      thumb_input_modes_[index] =
          index == 1 && mode_number && mode_number.integerValue == XeniaTouchStickInputModeTouchpad
              ? XeniaTouchStickInputModeTouchpad
              : XeniaTouchStickInputModeJoystick;
      NSString* title = [stick_config[@"title"] isKindOfClass:[NSString class]] ? stick_config[@"title"] : nil;
      [thumb_titles_[index] release];
      thumb_titles_[index] = title.length > 0 ? [title copy] : nil;
      UIColor* color = XeniaColorFromConfiguration(stick_config[@"color"]);
      [thumb_colors_[index] release];
      thumb_colors_[index] = [color retain];
    }
  }
  for (NSInteger tag = 1; tag <= 14; ++tag) {
    [self invalidateButtonAssetsForTag:tag];
  }
  [self invalidateMenuAssets];
  [self invalidateKnobAssets];
  [self setNeedsLayout];
}

- (void)resetLayoutConfigurationToDefaults {
  memset(button_offset_units_, 0, sizeof(button_offset_units_));
  memset(thumb_offset_units_, 0, sizeof(thumb_offset_units_));
  memset(button_rotations_, 0, sizeof(button_rotations_));
  memset(thumb_rotations_, 0, sizeof(thumb_rotations_));
  for (NSInteger tag = 1; tag <= 14; ++tag) {
    button_scales_[tag] = 1.0f;
    button_shapes_[tag] = XeniaDefaultShapeForControlIdentifier(tag);
    button_hidden_[tag] = NO;
  }
  thumb_scales_[0] = 1.0f;
  thumb_scales_[1] = 1.0f;
  thumb_shapes_[0] = XeniaDefaultShapeForControlIdentifier(kXeniaTouchControlIdentifierLeftStick);
  thumb_shapes_[1] = XeniaDefaultShapeForControlIdentifier(kXeniaTouchControlIdentifierRightStick);
  thumb_hidden_[0] = NO;
  thumb_hidden_[1] = NO;
  thumb_input_modes_[0] = XeniaTouchStickInputModeJoystick;
  thumb_input_modes_[1] = XeniaTouchStickInputModeJoystick;
  menu_offset_units_ = CGPointZero;
  menu_scale_ = 1.0f;
  menu_rotation_ = 0.0f;
  menu_shape_ = XeniaDefaultShapeForControlIdentifier(kXeniaTouchControlIdentifierMenu);
  menu_hidden_ = NO;
  for (NSInteger tag = 1; tag <= 14; ++tag) {
    [button_titles_[tag] release];
    button_titles_[tag] = nil;
    [button_colors_[tag] release];
    button_colors_[tag] = nil;
    [self invalidateButtonAssetsForTag:tag];
  }
  [thumb_titles_[0] release];
  thumb_titles_[0] = nil;
  [thumb_titles_[1] release];
  thumb_titles_[1] = nil;
  [thumb_colors_[0] release];
  thumb_colors_[0] = nil;
  [thumb_colors_[1] release];
  thumb_colors_[1] = nil;
  [menu_title_ release];
  menu_title_ = nil;
  [menu_color_ release];
  menu_color_ = nil;
  [self invalidateMenuAssets];
  [self invalidateKnobAssets];
  [self setNeedsLayout];
}

- (void)notifyLayoutEditingSelectionChanged {
  if (layoutEditingSelectionDidChangeHandler_) {
    layoutEditingSelectionDidChangeHandler_(selectedControlIdentifier_);
  }
}

- (void)notifyLayoutEditingDidChange {
  if (layoutEditingDidChangeHandler_) {
    layoutEditingDidChangeHandler_();
  }
}

- (void)queueLayoutEditingRefresh {
  [self setNeedsLayout];
  if (layoutEditingEnabled_) {
    [self notifyLayoutEditingDidChange];
  }
}

- (void)scheduleTouchpadAutoResetForIndex:(NSInteger)index {
  thumb_touchpad_activity_tokens_[index] += 1;
  uint32_t token = thumb_touchpad_activity_tokens_[index];
  XeniaTouchControlsView* retained_self = [self retain];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 24000000), dispatch_get_main_queue(), ^{
    if (retained_self->thumb_input_modes_[index] == XeniaTouchStickInputModeTouchpad &&
        retained_self->thumb_touchpad_activity_tokens_[index] == token) {
      [retained_self resetThumbIndex:index emit:YES];
    }
    [retained_self release];
  });
}

- (void)rotateSelectedControl:(UIRotationGestureRecognizer*)recognizer {
  if (!layoutEditingEnabled_ || selectedControlIdentifier_ < 0) {
    recognizer.rotation = 0.0f;
    return;
  }
  CGFloat delta = recognizer.rotation;
  if (fabs(delta) <= 0.0001f) {
    return;
  }
  if (selectedControlIdentifier_ >= 1 && selectedControlIdentifier_ <= 14) {
    button_rotations_[selectedControlIdentifier_] += delta;
    [self updateButtonLayerForTag:selectedControlIdentifier_];
  } else if (selectedControlIdentifier_ == kXeniaTouchControlIdentifierMenu) {
    menu_rotation_ += delta;
    [self updateMenuLayer];
  } else if (selectedControlIdentifier_ == kXeniaTouchControlIdentifierLeftStick) {
    thumb_rotations_[0] += delta;
    [self updateThumbLayerForIndex:0];
  } else if (selectedControlIdentifier_ == kXeniaTouchControlIdentifierRightStick) {
    thumb_rotations_[1] += delta;
    [self updateThumbLayerForIndex:1];
  }
  recognizer.rotation = 0.0f;
  [self notifyLayoutEditingDidChange];
}

- (CGRect)frameForControlIdentifier:(NSInteger)controlIdentifier {
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    return button_frames_[controlIdentifier];
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    return menu_frame_;
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick) {
    return thumb_frames_[0];
  }
  if (controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    return thumb_frames_[1];
  }
  return CGRectZero;
}

- (NSInteger)controlIdentifierForTouchPoint:(CGPoint)point {
  NSInteger button_tag = [self buttonTagForTouchPoint:point];
  if (button_tag >= 0) {
    return button_tag;
  }
  if ([self isPointInsideMenu:point]) {
    return kXeniaTouchControlIdentifierMenu;
  }
  NSInteger thumb_index = [self thumbIndexForTouchPoint:point];
  if (thumb_index == 0) {
    return kXeniaTouchControlIdentifierLeftStick;
  }
  if (thumb_index == 1) {
    return kXeniaTouchControlIdentifierRightStick;
  }
  return -1;
}

- (void)updateEditingHighlight {
  UIColor* highlight_color = [UIColor colorWithRed:0.96f green:0.76f blue:0.24f alpha:1.0f];
  for (NSInteger tag = 1; tag <= 14; ++tag) {
    CALayer* layer = button_layers_[tag];
    BOOL selected = layoutEditingEnabled_ && selectedControlIdentifier_ == tag;
    layer.shadowColor = highlight_color.CGColor;
    layer.shadowOpacity = selected ? 0.95f : 0.0f;
    layer.shadowRadius = selected ? 16.0f : 0.0f;
    layer.shadowOffset = CGSizeZero;
  }
  BOOL menu_selected = layoutEditingEnabled_ && selectedControlIdentifier_ == kXeniaTouchControlIdentifierMenu;
  menu_layer_.shadowColor = highlight_color.CGColor;
  menu_layer_.shadowOpacity = menu_selected ? 0.95f : 0.0f;
  menu_layer_.shadowRadius = menu_selected ? 16.0f : 0.0f;
  menu_layer_.shadowOffset = CGSizeZero;
  for (NSInteger index = 0; index < 2; ++index) {
    BOOL selected = layoutEditingEnabled_ &&
                    selectedControlIdentifier_ ==
                        (index == 0 ? kXeniaTouchControlIdentifierLeftStick
                                    : kXeniaTouchControlIdentifierRightStick);
    thumb_base_layers_[index].shadowColor = highlight_color.CGColor;
    thumb_base_layers_[index].shadowOpacity = selected ? 0.75f : 0.0f;
    thumb_base_layers_[index].shadowRadius = selected ? 18.0f : 0.0f;
    thumb_base_layers_[index].shadowOffset = CGSizeZero;
    thumb_layers_[index].shadowColor = highlight_color.CGColor;
    thumb_layers_[index].shadowOpacity = selected ? 0.95f : 0.0f;
    thumb_layers_[index].shadowRadius = selected ? 16.0f : 0.0f;
    thumb_layers_[index].shadowOffset = CGSizeZero;
  }
}

- (BOOL)applySnapForSelectedControlWithShortSide:(CGFloat)short_side {
  if (selectedControlIdentifier_ < 0 || short_side <= 0.0f) {
    return NO;
  }
  CGRect selected_frame = [self frameForControlIdentifier:selectedControlIdentifier_];
  if (CGRectIsEmpty(selected_frame)) {
    return NO;
  }
  constexpr CGFloat kSnapTolerance = 4.0f;
  constexpr CGFloat kStrongSnapTolerance = 2.0f;
  CGFloat selected_center_x = CGRectGetMidX(selected_frame);
  CGFloat selected_center_y = CGRectGetMidY(selected_frame);
  CGFloat best_dx = kSnapTolerance + 1.0f;
  CGFloat best_dy = kSnapTolerance + 1.0f;
  const NSInteger identifiers[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    kXeniaTouchControlIdentifierMenu,
    kXeniaTouchControlIdentifierLeftStick,
    kXeniaTouchControlIdentifierRightStick,
  };
  for (NSInteger identifier_index = 0;
       identifier_index < static_cast<NSInteger>(sizeof(identifiers) / sizeof(identifiers[0]));
       ++identifier_index) {
    NSInteger control_identifier = identifiers[identifier_index];
    if (control_identifier == selectedControlIdentifier_) {
      continue;
    }
    CGRect other_frame = [self frameForControlIdentifier:control_identifier];
    if (CGRectIsEmpty(other_frame)) {
      continue;
    }
    CGFloat dx = CGRectGetMidX(other_frame) - selected_center_x;
    CGFloat dy = CGRectGetMidY(other_frame) - selected_center_y;
    if (fabs(dx) < fabs(best_dx) && fabs(dx) <= kSnapTolerance) {
      best_dx = dx;
    }
    if (fabs(dy) < fabs(best_dy) && fabs(dy) <= kSnapTolerance) {
      best_dy = dy;
    }
  }
  BOOL snapped = NO;
  CGPoint delta_units = CGPointZero;
  if (fabs(best_dx) <= kStrongSnapTolerance) {
    delta_units.x = best_dx / short_side;
    snapped = YES;
  }
  if (fabs(best_dy) <= kStrongSnapTolerance) {
    delta_units.y = best_dy / short_side;
    snapped = YES;
  }
  if (!snapped) {
    return NO;
  }
  if (selectedControlIdentifier_ >= 1 && selectedControlIdentifier_ <= 14) {
    button_offset_units_[selectedControlIdentifier_].x += delta_units.x;
    button_offset_units_[selectedControlIdentifier_].y += delta_units.y;
  } else if (selectedControlIdentifier_ == kXeniaTouchControlIdentifierMenu) {
    menu_offset_units_.x += delta_units.x;
    menu_offset_units_.y += delta_units.y;
  } else if (selectedControlIdentifier_ == kXeniaTouchControlIdentifierLeftStick) {
    thumb_offset_units_[0].x += delta_units.x;
    thumb_offset_units_[0].y += delta_units.y;
  } else if (selectedControlIdentifier_ == kXeniaTouchControlIdentifierRightStick) {
    thumb_offset_units_[1].x += delta_units.x;
    thumb_offset_units_[1].y += delta_units.y;
  }
  return YES;
}

- (CGRect)applyCustomizationToDefaultFrame:(CGRect)default_frame
                          controlIdentifier:(NSInteger)controlIdentifier
                                   minScale:(CGFloat)min_scale
                                   maxScale:(CGFloat)max_scale
                                  shortSide:(CGFloat)short_side
                                 safeInsets:(UIEdgeInsets)insets {
  CGPoint offset_units = CGPointZero;
  CGFloat scale = 1.0f;
  NSInteger shape = XeniaDefaultShapeForControlIdentifier(controlIdentifier);
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    offset_units = button_offset_units_[controlIdentifier];
    scale = button_scales_[controlIdentifier];
    shape = button_shapes_[controlIdentifier];
  } else if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    offset_units = menu_offset_units_;
    scale = menu_scale_;
    shape = menu_shape_;
  } else if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick) {
    offset_units = thumb_offset_units_[0];
    scale = thumb_scales_[0];
    shape = thumb_shapes_[0];
  } else if (controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    offset_units = thumb_offset_units_[1];
    scale = thumb_scales_[1];
    shape = thumb_shapes_[1];
  }
  scale = XeniaClamp(scale, min_scale, max_scale);
  CGRect shaped_default =
      XeniaDefaultFrameAdjustedForShape(default_frame, controlIdentifier,
                                        static_cast<XeniaTouchControlShape>(shape));
  CGRect scaled = XeniaScaledRect(shaped_default, scale);
  scaled.origin.x += offset_units.x * short_side;
  scaled.origin.y += offset_units.y * short_side;
  CGRect clamped = XeniaClampRectToBounds(scaled, self.bounds, insets);
  CGPoint normalized_offset = CGPointZero;
  if (short_side > 0.0f) {
    normalized_offset = CGPointMake((CGRectGetMidX(clamped) - CGRectGetMidX(shaped_default)) / short_side,
                                    (CGRectGetMidY(clamped) - CGRectGetMidY(shaped_default)) / short_side);
  }
  if (controlIdentifier >= 1 && controlIdentifier <= 14) {
    button_offset_units_[controlIdentifier] = normalized_offset;
    button_scales_[controlIdentifier] = scale;
  } else if (controlIdentifier == kXeniaTouchControlIdentifierMenu) {
    menu_offset_units_ = normalized_offset;
    menu_scale_ = scale;
  } else if (controlIdentifier == kXeniaTouchControlIdentifierLeftStick) {
    thumb_offset_units_[0] = normalized_offset;
    thumb_scales_[0] = scale;
  } else if (controlIdentifier == kXeniaTouchControlIdentifierRightStick) {
    thumb_offset_units_[1] = normalized_offset;
    thumb_scales_[1] = scale;
  }
  return clamped;
}

- (void)setDigitalButton:(uint16_t)mask pressed:(BOOL)pressed {
  uint16_t buttons = static_cast<uint16_t>(state_.gamepad.buttons);
  buttons = pressed ? static_cast<uint16_t>(buttons | mask)
                    : static_cast<uint16_t>(buttons & static_cast<uint16_t>(~mask));
  state_.gamepad.buttons = buttons;
}

- (BOOL)isButtonPressed:(NSInteger)tag {
  switch (tag) {
    case 1:
      return state_.gamepad.left_trigger > 0;
    case 2:
      return (state_.gamepad.buttons & xe::hid::X_INPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    case 3:
      return (state_.gamepad.buttons & xe::hid::X_INPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    case 4:
      return state_.gamepad.right_trigger > 0;
    case 5:
      return (state_.gamepad.buttons & xe::hid::X_INPUT_GAMEPAD_DPAD_UP) != 0;
    case 6:
      return (state_.gamepad.buttons & xe::hid::X_INPUT_GAMEPAD_DPAD_DOWN) != 0;
    case 7:
      return (state_.gamepad.buttons & xe::hid::X_INPUT_GAMEPAD_DPAD_LEFT) != 0;
    case 8:
      return (state_.gamepad.buttons & xe::hid::X_INPUT_GAMEPAD_DPAD_RIGHT) != 0;
    case 9:
      return (state_.gamepad.buttons & xe::hid::X_INPUT_GAMEPAD_X) != 0;
    case 10:
      return (state_.gamepad.buttons & xe::hid::X_INPUT_GAMEPAD_Y) != 0;
    case 11:
      return (state_.gamepad.buttons & xe::hid::X_INPUT_GAMEPAD_A) != 0;
    case 12:
      return (state_.gamepad.buttons & xe::hid::X_INPUT_GAMEPAD_B) != 0;
    case 13:
      return (state_.gamepad.buttons & xe::hid::X_INPUT_GAMEPAD_BACK) != 0;
    case 14:
      return (state_.gamepad.buttons & xe::hid::X_INPUT_GAMEPAD_START) != 0;
    default:
      return NO;
  }
}

- (void)setButtonTag:(NSInteger)tag pressed:(BOOL)pressed emit:(BOOL)emit {
  if (tag < 1 || tag > 14 || button_pressed_[tag] == pressed) {
    return;
  }
  button_pressed_[tag] = pressed;
  switch (tag) {
    case 1: state_.gamepad.left_trigger = pressed ? 0xFF : 0; break;
    case 2: [self setDigitalButton:xe::hid::X_INPUT_GAMEPAD_LEFT_SHOULDER pressed:pressed]; break;
    case 3: [self setDigitalButton:xe::hid::X_INPUT_GAMEPAD_RIGHT_SHOULDER pressed:pressed]; break;
    case 4: state_.gamepad.right_trigger = pressed ? 0xFF : 0; break;
    case 5: [self setDigitalButton:xe::hid::X_INPUT_GAMEPAD_DPAD_UP pressed:pressed]; break;
    case 6: [self setDigitalButton:xe::hid::X_INPUT_GAMEPAD_DPAD_DOWN pressed:pressed]; break;
    case 7: [self setDigitalButton:xe::hid::X_INPUT_GAMEPAD_DPAD_LEFT pressed:pressed]; break;
    case 8: [self setDigitalButton:xe::hid::X_INPUT_GAMEPAD_DPAD_RIGHT pressed:pressed]; break;
    case 9: [self setDigitalButton:xe::hid::X_INPUT_GAMEPAD_X pressed:pressed]; break;
    case 10: [self setDigitalButton:xe::hid::X_INPUT_GAMEPAD_Y pressed:pressed]; break;
    case 11: [self setDigitalButton:xe::hid::X_INPUT_GAMEPAD_A pressed:pressed]; break;
    case 12: [self setDigitalButton:xe::hid::X_INPUT_GAMEPAD_B pressed:pressed]; break;
    case 13: [self setDigitalButton:xe::hid::X_INPUT_GAMEPAD_BACK pressed:pressed]; break;
    case 14: [self setDigitalButton:xe::hid::X_INPUT_GAMEPAD_START pressed:pressed]; break;
    default: break;
  }
  [self updateButtonLayerForTag:tag];
  if (!layoutEditingEnabled_) {
    [self invalidateGameplayControlsComposite];
    [self updateGameplayControlsLayer];
  }
  if (emit) {
    [self emitStateChanged];
  }
}

- (void)setMenuPressed:(BOOL)pressed {
  if (menu_pressed_ == pressed) {
    return;
  }
  menu_pressed_ = pressed;
  [self updateMenuLayer];
  if (!layoutEditingEnabled_) {
    [self invalidateGameplayControlsComposite];
    [self updateGameplayControlsLayer];
  }
}

- (void)updateThumbIndex:(NSInteger)index forPoint:(CGPoint)point emit:(BOOL)emit {
  CGFloat radius = XeniaThumbRadius(thumb_frames_[index]);
  if (radius <= 0.0f) {
    return;
  }
  ThumbVector next_vector = thumb_vectors_[index];
  if (thumb_input_modes_[index] == XeniaTouchStickInputModeTouchpad) {
    CGPoint previous = thumb_touch_last_points_[index];
    if (CGPointEqualToPoint(previous, CGPointZero)) {
      previous = point;
    }
    CGFloat sensitivity = 100.0f;
    next_vector.x = ((point.x - previous.x) / radius) * sensitivity;
    next_vector.y = ((previous.y - point.y) / radius) * sensitivity;
    CGFloat length = std::sqrt(next_vector.x * next_vector.x + next_vector.y * next_vector.y);
    if (length > 1.0f) {
      next_vector.x /= length;
      next_vector.y /= length;
    }
    thumb_touch_last_points_[index] = point;
  } else {
    CGPoint center = XeniaThumbCenter(thumb_frames_[index]);
    CGFloat dx = point.x - center.x;
    CGFloat dy = center.y - point.y;
    CGFloat length = std::sqrt(dx * dx + dy * dy);
    if (length > radius) {
      dx = dx / length * radius;
      dy = dy / length * radius;
    }
    next_vector = ThumbVector(dx / radius, dy / radius);
  }
  if (std::fabs(next_vector.x - thumb_vectors_[index].x) < 0.0005f &&
      std::fabs(next_vector.y - thumb_vectors_[index].y) < 0.0005f) {
    return;
  }
  thumb_vectors_[index] = next_vector;
  if (index == 0) {
    state_.gamepad.thumb_lx = XeniaThumbAxis(next_vector.x);
    state_.gamepad.thumb_ly = XeniaThumbAxis(next_vector.y);
  } else {
    state_.gamepad.thumb_rx = XeniaThumbAxis(next_vector.x);
    state_.gamepad.thumb_ry = XeniaThumbAxis(next_vector.y);
  }
  [self updateThumbLayerForIndex:index];
  if (emit) {
    [self emitStateChanged];
  }
  if (thumb_input_modes_[index] == XeniaTouchStickInputModeTouchpad) {
    [self scheduleTouchpadAutoResetForIndex:index];
  }
}

- (void)updateTouchpadIndex:(NSInteger)index
                 withSamples:(NSArray<UITouch*>*)samples
                        emit:(BOOL)emit {
  if (samples.count == 0) {
    return;
  }
  CGFloat radius = XeniaThumbRadius(thumb_frames_[index]);
  if (radius <= 0.0f) {
    return;
  }

  CGFloat accumulated_dx = 0.0f;
  CGFloat accumulated_dy = 0.0f;
  NSUInteger accumulated_count = 0;

  for (UITouch* sample in samples) {
    CGPoint point = XeniaTouchLocation(sample, self);
    CGPoint previous_point = [sample precisePreviousLocationInView:self];
    accumulated_dx += point.x - previous_point.x;
    accumulated_dy += previous_point.y - point.y;
    accumulated_count += 1;
    thumb_touch_last_points_[index] = point;
  }

  if (accumulated_count == 0) {
    return;
  }

  CGFloat total_dx = accumulated_dx;
  CGFloat total_dy = accumulated_dy;
  CGFloat sensitivity = kXeniaRightTouchpadSensitivity;
  CGFloat pixels_for_full_scale = std::max<CGFloat>(0.4f, 900.0f / sensitivity);
  static constexpr CGFloat kDeltaDeadzone = 0.0f;
  ThumbVector target_vector;
  target_vector.x = total_dx / pixels_for_full_scale;
  target_vector.y = total_dy / pixels_for_full_scale;
  if (std::fabs(total_dx) < kDeltaDeadzone) {
    target_vector.x = 0.0f;
  }
  if (std::fabs(total_dy) < kDeltaDeadzone) {
    target_vector.y = 0.0f;
  }
  CGFloat length = std::sqrt(target_vector.x * target_vector.x + target_vector.y * target_vector.y);
  if (length > 0.0005f) {
    CGFloat clamped_length = std::min<CGFloat>(1.0f, length);
    CGFloat adjusted_length = 0.5f * std::sqrt(clamped_length) + 0.5f * clamped_length * clamped_length;
    CGFloat scale = adjusted_length / length;
    target_vector.x *= scale;
    target_vector.y *= scale;
  }
  static constexpr CGFloat kTouchpadSmoothing = 0.38f;
  ThumbVector next_vector;
  next_vector.x = thumb_vectors_[index].x + (target_vector.x - thumb_vectors_[index].x) * kTouchpadSmoothing;
  next_vector.y = thumb_vectors_[index].y + (target_vector.y - thumb_vectors_[index].y) * kTouchpadSmoothing;
  if (std::fabs(next_vector.x - thumb_vectors_[index].x) < 0.0005f &&
      std::fabs(next_vector.y - thumb_vectors_[index].y) < 0.0005f) {
    return;
  }

  thumb_vectors_[index] = next_vector;
  state_.gamepad.thumb_rx = XeniaThumbAxis(next_vector.x);
  state_.gamepad.thumb_ry = XeniaThumbAxis(next_vector.y);
  [self updateThumbLayerForIndex:index];
  if (emit) {
    [self emitStateChanged];
  }
  [self scheduleTouchpadAutoResetForIndex:index];
}

- (void)emitThumbClickForIndex:(NSInteger)index {
  const uint16_t mask = index == 0 ? xe::hid::X_INPUT_GAMEPAD_LEFT_THUMB
                                   : xe::hid::X_INPUT_GAMEPAD_RIGHT_THUMB;
  [self setDigitalButton:mask pressed:YES];
  [self emitStateChanged];
  XeniaTouchControlsView* retained_self = [self retain];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50000000), dispatch_get_main_queue(), ^{
    [retained_self setDigitalButton:mask pressed:NO];
    [retained_self emitStateChanged];
    [retained_self release];
  });
}

- (void)resetThumbIndex:(NSInteger)index emit:(BOOL)emit {
  if (std::fabs(thumb_vectors_[index].x) < 0.0005f &&
      std::fabs(thumb_vectors_[index].y) < 0.0005f) {
    return;
  }
  thumb_vectors_[index] = ThumbVector();
  thumb_touch_last_points_[index] = CGPointZero;
  thumb_touchpad_activity_tokens_[index] += 1;
  if (index == 0) {
    state_.gamepad.thumb_lx = 0;
    state_.gamepad.thumb_ly = 0;
  } else {
    state_.gamepad.thumb_rx = 0;
    state_.gamepad.thumb_ry = 0;
  }
  [self updateThumbLayerForIndex:index];
  if (emit) {
    [self emitStateChanged];
  }
}

- (NSInteger)buttonTagForTouchPoint:(CGPoint)point {
  for (NSInteger tag = 1; tag <= 14; ++tag) {
    if (button_hidden_[tag] && !layoutEditingEnabled_) {
      continue;
    }
    if (CGRectContainsPoint(button_frames_[tag], point)) {
      return tag;
    }
  }
  return -1;
}

- (NSInteger)thumbIndexForTouchPoint:(CGPoint)point {
  for (NSInteger index = 0; index < 2; ++index) {
    if (thumb_hidden_[index] && !layoutEditingEnabled_) {
      continue;
    }
    CGRect hit_rect = CGRectZero;
    if (layoutEditingEnabled_ && thumb_input_modes_[index] != XeniaTouchStickInputModeTouchpad) {
      hit_rect = CGRectInset(thumb_frames_[index], -12.0f, -12.0f);
    } else {
      hit_rect = [self thumbHitRectForIndex:index];
    }
    if (CGRectContainsPoint(hit_rect, point)) {
      return index;
    }
  }
  return -1;
}

- (BOOL)isPointInsideMenu:(CGPoint)point {
  if (menu_hidden_ && !layoutEditingEnabled_) {
    return NO;
  }
  return CGRectContainsPoint(menu_frame_, point);
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  if (layoutEditingEnabled_) {
    if (event.allTouches.count > 1) {
      return;
    }
    for (UITouch* touch in touches) {
      CGPoint point = [touch locationInView:self];
      NSInteger control_identifier = [self controlIdentifierForTouchPoint:point];
      layout_edit_previous_selected_control_identifier_ = selectedControlIdentifier_;
      layout_edit_touch_start_control_identifier_ = control_identifier;
      layout_edit_touch_start_point_ = point;
      layout_edit_touch_moved_ = NO;
      if (control_identifier >= 0 && control_identifier != selectedControlIdentifier_) {
        selectedControlIdentifier_ = control_identifier;
        [self updateEditingHighlight];
        [self notifyLayoutEditingSelectionChanged];
      }
      if (control_identifier >= 0 && layout_edit_touch_ == nil) {
        layout_edit_touch_ = touch;
        layout_edit_last_point_ = point;
      }
    }
    return;
  }
  for (UITouch* touch in touches) {
    CGPoint point = XeniaTouchLocation(touch, self);
    if ([self isPointInsideMenu:point] && menu_touch_ == nil) {
      menu_touch_ = touch;
      [self setMenuPressed:YES];
      continue;
    }

    NSInteger tag = [self buttonTagForTouchPoint:point];
    if (tag >= 0 && button_touches_[tag] == nil) {
      button_touches_[tag] = touch;
      [self setButtonTag:tag pressed:YES emit:YES];
      continue;
    }

    NSInteger thumb_index = [self thumbIndexForTouchPoint:point];
    if (thumb_index >= 0 && thumb_touches_[thumb_index] == nil) {
      thumb_touches_[thumb_index] = touch;
      thumb_touch_start_points_[thumb_index] = point;
      thumb_touch_last_points_[thumb_index] = point;
      thumb_touch_dragged_[thumb_index] = NO;
      if (thumb_input_modes_[thumb_index] == XeniaTouchStickInputModeJoystick) {
        [self updateThumbIndex:thumb_index forPoint:point emit:YES];
      }
    }
  }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  if (layoutEditingEnabled_) {
    if (event.allTouches.count > 1) {
      return;
    }
    for (UITouch* touch in touches) {
      if (touch != layout_edit_touch_ || selectedControlIdentifier_ < 0) {
        continue;
      }
      CGPoint point = [touch locationInView:self];
      CGFloat short_side = MIN(CGRectGetWidth(self.bounds), CGRectGetHeight(self.bounds));
      if (short_side <= 0.0f) {
        layout_edit_last_point_ = point;
        continue;
      }
      CGPoint drag_delta = CGPointMake(point.x - layout_edit_touch_start_point_.x,
                                       point.y - layout_edit_touch_start_point_.y);
      if ((drag_delta.x * drag_delta.x + drag_delta.y * drag_delta.y) > (6.0f * 6.0f)) {
        layout_edit_touch_moved_ = YES;
      }
      CGPoint delta = CGPointMake((point.x - layout_edit_last_point_.x) / short_side,
                                  (point.y - layout_edit_last_point_.y) / short_side);
      if (selectedControlIdentifier_ >= 1 && selectedControlIdentifier_ <= 14) {
        button_offset_units_[selectedControlIdentifier_].x += delta.x;
        button_offset_units_[selectedControlIdentifier_].y += delta.y;
      } else if (selectedControlIdentifier_ == kXeniaTouchControlIdentifierMenu) {
        menu_offset_units_.x += delta.x;
        menu_offset_units_.y += delta.y;
      } else if (selectedControlIdentifier_ == kXeniaTouchControlIdentifierLeftStick) {
        thumb_offset_units_[0].x += delta.x;
        thumb_offset_units_[0].y += delta.y;
      } else if (selectedControlIdentifier_ == kXeniaTouchControlIdentifierRightStick) {
        thumb_offset_units_[1].x += delta.x;
        thumb_offset_units_[1].y += delta.y;
      }
      layout_edit_last_point_ = point;
      [self setNeedsLayout];
      [self layoutIfNeeded];
      if ([self applySnapForSelectedControlWithShortSide:short_side]) {
        [self setNeedsLayout];
        [self layoutIfNeeded];
      }
      [self notifyLayoutEditingDidChange];
    }
    return;
  }
  for (UITouch* touch in touches) {
    CGPoint point = XeniaTouchLocation(touch, self);
    for (NSInteger index = 0; index < 2; ++index) {
      if (thumb_touches_[index] == touch) {
        NSArray<UITouch*>* samples = [event coalescedTouchesForTouch:touch];
        if (samples.count == 0) {
          samples = @[ touch ];
        }
        if (thumb_input_modes_[index] == XeniaTouchStickInputModeTouchpad) {
          [self updateTouchpadIndex:index withSamples:samples emit:YES];
        } else {
          for (NSUInteger sample_index = 0; sample_index < samples.count; ++sample_index) {
            UITouch* sample = samples[sample_index];
            CGPoint sample_point = XeniaTouchLocation(sample, self);
            CGPoint start = thumb_touch_start_points_[index];
            CGFloat dx = sample_point.x - start.x;
            CGFloat dy = sample_point.y - start.y;
          CGFloat drag_threshold =
              thumb_input_modes_[index] == XeniaTouchStickInputModeTouchpad ? 4.0f : 12.0f;
          if ((dx * dx + dy * dy) > (drag_threshold * drag_threshold)) {
            thumb_touch_dragged_[index] = YES;
          }
            [self updateThumbIndex:index
                          forPoint:sample_point
                              emit:sample_index + 1 == samples.count];
          }
        }
        goto next_touch;
      }
    }

    if (menu_touch_ == touch) {
      BOOL inside = [self isPointInsideMenu:point];
      [self setMenuPressed:inside];
      goto next_touch;
    }

    for (NSInteger tag = 1; tag <= 14; ++tag) {
      if (button_touches_[tag] == touch) {
        BOOL inside = CGRectContainsPoint(button_frames_[tag], point);
        [self setButtonTag:tag pressed:inside emit:YES];
        goto next_touch;
      }
    }

  next_touch:;
  }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  (void)event;
  if (layoutEditingEnabled_) {
    for (UITouch* touch in touches) {
      if (touch == layout_edit_touch_) {
        if (!layout_edit_touch_moved_ &&
            layout_edit_touch_start_control_identifier_ >= 0 &&
            layout_edit_touch_start_control_identifier_ ==
                layout_edit_previous_selected_control_identifier_ &&
            selectedControlIdentifier_ == layout_edit_touch_start_control_identifier_) {
          selectedControlIdentifier_ = -1;
          [self updateEditingHighlight];
          [self notifyLayoutEditingSelectionChanged];
        }
        layout_edit_touch_ = nil;
        layout_edit_last_point_ = CGPointZero;
        layout_edit_touch_start_point_ = CGPointZero;
        layout_edit_touch_start_control_identifier_ = -1;
        layout_edit_previous_selected_control_identifier_ = -1;
        layout_edit_touch_moved_ = NO;
      }
    }
    return;
  }
  for (UITouch* touch in touches) {
    CGPoint point = [touch locationInView:self];
    for (NSInteger index = 0; index < 2; ++index) {
      if (thumb_touches_[index] == touch) {
        CGFloat dx = point.x - thumb_touch_start_points_[index].x;
        CGFloat dy = point.y - thumb_touch_start_points_[index].y;
        CGFloat tap_threshold =
            thumb_input_modes_[index] == XeniaTouchStickInputModeTouchpad ? 4.0f : 12.0f;
        BOOL should_click = !thumb_touch_dragged_[index] &&
                            (dx * dx + dy * dy) <= (tap_threshold * tap_threshold) &&
                            CGRectContainsPoint([self thumbHitRectForIndex:index], point);
        thumb_touches_[index] = nil;
        thumb_touch_start_points_[index] = CGPointZero;
        thumb_touch_last_points_[index] = CGPointZero;
        thumb_touch_dragged_[index] = NO;
        [self resetThumbIndex:index emit:YES];
        if (should_click) {
          [self emitThumbClickForIndex:index];
        }
        goto next_touch;
      }
    }

    if (menu_touch_ == touch) {
      BOOL activate = menu_pressed_;
      menu_touch_ = nil;
      [self setMenuPressed:NO];
      if (activate && menuButtonTappedHandler_) {
        menuButtonTappedHandler_();
      }
      goto next_touch;
    }

    for (NSInteger tag = 1; tag <= 14; ++tag) {
      if (button_touches_[tag] == touch) {
        button_touches_[tag] = nil;
        [self setButtonTag:tag pressed:NO emit:YES];
        goto next_touch;
      }
    }

  next_touch:;
  }
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  if (layoutEditingEnabled_) {
    for (UITouch* touch in touches) {
      if (touch == layout_edit_touch_) {
        layout_edit_touch_ = nil;
        layout_edit_last_point_ = CGPointZero;
        layout_edit_touch_start_point_ = CGPointZero;
        layout_edit_touch_start_control_identifier_ = -1;
        layout_edit_previous_selected_control_identifier_ = -1;
        layout_edit_touch_moved_ = NO;
      }
    }
    return;
  }
  [self touchesEnded:touches withEvent:event];
}

@end
