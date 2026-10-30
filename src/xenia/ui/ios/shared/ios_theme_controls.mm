/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_theme_controls.h"

#import "xenia/ui/ios/shared/ios_theme.h"

@implementation XeniaPaddedLabel

- (CGSize)intrinsicContentSize {
  CGSize size = [super intrinsicContentSize];
  return CGSizeMake(size.width + _padding.left + _padding.right,
                    size.height + _padding.top + _padding.bottom);
}

- (void)drawTextInRect:(CGRect)rect {
  [super drawTextInRect:UIEdgeInsetsInsetRect(rect, _padding)];
}

- (CGRect)textRectForBounds:(CGRect)bounds limitedToNumberOfLines:(NSInteger)numberOfLines {
  CGRect inset = UIEdgeInsetsInsetRect(bounds, _padding);
  CGRect result = [super textRectForBounds:inset limitedToNumberOfLines:numberOfLines];
  result.origin.x -= _padding.left;
  result.origin.y -= _padding.top;
  result.size.width += _padding.left + _padding.right;
  result.size.height += _padding.top + _padding.bottom;
  return result;
}

@end

UIFont* xe_scaled_system_font(UIFontTextStyle text_style, CGFloat point_size, UIFontWeight weight) {
  UIFont* base_font = [UIFont systemFontOfSize:point_size weight:weight];
  return [[UIFontMetrics metricsForTextStyle:text_style] scaledFontForFont:base_font];
}

UIFont* xe_scaled_monospaced_font(UIFontTextStyle text_style, CGFloat point_size,
                                  UIFontWeight weight) {
  UIFont* base_font = [UIFont monospacedSystemFontOfSize:point_size weight:weight];
  return [[UIFontMetrics metricsForTextStyle:text_style] scaledFontForFont:base_font];
}

void xe_apply_label_font(UILabel* label, UIFontTextStyle text_style, CGFloat point_size,
                         UIFontWeight weight) {
  if (!label) {
    return;
  }
  label.font = xe_scaled_system_font(text_style, point_size, weight);
  if (@available(iOS 10.0, *)) {
    label.adjustsFontForContentSizeCategory = YES;
  }
}

void xe_apply_monospaced_label_font(UILabel* label, UIFontTextStyle text_style, CGFloat point_size,
                                    UIFontWeight weight) {
  if (!label) {
    return;
  }
  label.font = xe_scaled_monospaced_font(text_style, point_size, weight);
  if (@available(iOS 10.0, *)) {
    label.adjustsFontForContentSizeCategory = YES;
  }
}

void xe_apply_button_title_font(UIButton* button, UIFontTextStyle text_style, CGFloat point_size,
                                UIFontWeight weight) {
  if (!button.titleLabel) {
    return;
  }
  button.titleLabel.font = xe_scaled_system_font(text_style, point_size, weight);
  if (@available(iOS 10.0, *)) {
    button.titleLabel.adjustsFontForContentSizeCategory = YES;
  }
}

void xe_apply_text_view_font(UITextView* text_view, UIFontTextStyle text_style, CGFloat point_size,
                             UIFontWeight weight, BOOL monospaced) {
  if (!text_view) {
    return;
  }
  text_view.font = monospaced ? xe_scaled_monospaced_font(text_style, point_size, weight)
                              : xe_scaled_system_font(text_style, point_size, weight);
  if (@available(iOS 10.0, *)) {
    text_view.adjustsFontForContentSizeCategory = YES;
  }
}

XeniaPaddedLabel* xe_make_tag_pill(NSString* text, UIColor* text_color) {
  XeniaPaddedLabel* pill = [[[XeniaPaddedLabel alloc] init] autorelease];
  pill.translatesAutoresizingMaskIntoConstraints = NO;
  pill.padding = UIEdgeInsetsMake(1, 6, 1, 6);
  xe_apply_label_font(pill, UIFontTextStyleCaption2, 10.0, UIFontWeightMedium);
  pill.text = text ?: @"";
  pill.textColor = text_color ?: [XeniaTheme textMuted];
  pill.backgroundColor = [(text_color ?: [XeniaTheme textMuted]) colorWithAlphaComponent:0.1];
  pill.layer.cornerRadius = 6.0;
  pill.clipsToBounds = YES;
  [pill setContentHuggingPriority:UILayoutPriorityRequired
                          forAxis:UILayoutConstraintAxisHorizontal];
  [pill setContentCompressionResistancePriority:UILayoutPriorityRequired
                                        forAxis:UILayoutConstraintAxisHorizontal];
  return pill;
}

// UIButton subclass that re-resolves its layer.borderColor when the host's
// userInterfaceStyle flips. CGColor is captured at assignment, so a plain
// dynamic-color initial assignment would freeze its dark variant for the
// lifetime of the button. Override traitCollectionDidChange: to refresh.
@interface XeniaSheetCloseButton : UIButton
@end

@implementation XeniaSheetCloseButton
- (void)traitCollectionDidChange:(UITraitCollection*)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if ([self.traitCollection
          hasDifferentColorAppearanceComparedToTraitCollection:previousTraitCollection]) {
    self.layer.borderColor =
        [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
          return (traits.userInterfaceStyle == UIUserInterfaceStyleLight)
                     ? [[UIColor blackColor] colorWithAlphaComponent:0.12]
                     : [[UIColor whiteColor] colorWithAlphaComponent:0.12];
        }].CGColor;
  }
}
@end

UIButton* xe_make_ios_sheet_close_button(id target, SEL action) {
  XeniaSheetCloseButton* button = [XeniaSheetCloseButton buttonWithType:UIButtonTypeSystem];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.backgroundColor = [[XeniaTheme bgSurface] colorWithAlphaComponent:0.76];
  button.layer.cornerRadius = 24.0;
  button.layer.borderWidth = 0.5;
  button.layer.borderColor =
      [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
        return (traits.userInterfaceStyle == UIUserInterfaceStyleLight)
                   ? [[UIColor blackColor] colorWithAlphaComponent:0.12]
                   : [[UIColor whiteColor] colorWithAlphaComponent:0.12];
      }].CGColor;
  UIImageSymbolConfiguration* config =
      [UIImageSymbolConfiguration configurationWithPointSize:22 weight:UIImageSymbolWeightSemibold];
  UIImage* image =
      [[UIImage systemImageNamed:@"xmark.circle"] imageByApplyingSymbolConfiguration:config];
  [button setImage:image forState:UIControlStateNormal];
  button.tintColor = [XeniaTheme accent];
  [button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
  [NSLayoutConstraint activateConstraints:@[
    [button.widthAnchor constraintEqualToConstant:48.0],
    [button.heightAnchor constraintEqualToConstant:48.0],
  ]];
  return button;
}

static UIImage* xe_scaled_settings_footer_image(UIImage* image) {
  if (!image) {
    return nil;
  }
  CGSize image_size = image.size;
  if (image_size.width <= 0.0 || image_size.height <= 0.0) {
    return image;
  }
  constexpr CGFloat kMaxIconSide = 24.0;
  CGFloat scale = MIN(kMaxIconSide / image_size.width, kMaxIconSide / image_size.height);
  if (scale >= 1.0) {
    return image;
  }

  CGSize target_size =
      CGSizeMake(ceil(image_size.width * scale), ceil(image_size.height * scale));
  UIGraphicsImageRendererFormat* format = [UIGraphicsImageRendererFormat preferredFormat];
  format.opaque = NO;
  UIGraphicsImageRenderer* renderer =
      [[UIGraphicsImageRenderer alloc] initWithSize:target_size format:format];
  UIImage* scaled = [renderer imageWithActions:^(UIGraphicsImageRendererContext* __unused ctx) {
    [image drawInRect:CGRectMake(0.0, 0.0, target_size.width, target_size.height)];
  }];
  [renderer release];
  return scaled;
}

UIImage* xe_settings_footer_image(NSString* asset_name, NSString* fallback_symbol_name,
                                  BOOL tintable) {
  UIImage* image = asset_name.length ? [UIImage imageNamed:asset_name] : nil;
  BOOL used_fallback = NO;
  if (!image) {
    used_fallback = YES;
    UIImageSymbolConfiguration* config =
        [UIImageSymbolConfiguration configurationWithPointSize:18.0
                                                        weight:UIImageSymbolWeightMedium];
    image = [UIImage systemImageNamed:fallback_symbol_name withConfiguration:config];
    if (!image) {
      image = [UIImage systemImageNamed:@"questionmark.circle" withConfiguration:config];
    }
  }
  image = xe_scaled_settings_footer_image(image);
  return [image imageWithRenderingMode:(tintable || used_fallback)
                                           ? UIImageRenderingModeAlwaysTemplate
                                           : UIImageRenderingModeAlwaysOriginal];
}

UIButton* xe_make_settings_footer_button(NSString* asset_name, NSString* fallback_symbol_name,
                                         NSString* accessibility_label, NSInteger tag,
                                         BOOL tintable, id target, SEL action) {
  UIImage* image = xe_settings_footer_image(asset_name, fallback_symbol_name, tintable);
  UIButton* button = [UIButton buttonWithType:UIButtonTypeSystem];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.tag = tag;
  button.backgroundColor = [UIColor clearColor];
  button.tintColor = [XeniaTheme textPrimary];
  UIButtonConfiguration* configuration = [UIButtonConfiguration plainButtonConfiguration];
  configuration.image = image;
  configuration.title = nil;
  configuration.imagePadding = 0.0;
  configuration.contentInsets = NSDirectionalEdgeInsetsMake(6.0, 8.0, 6.0, 8.0);
  configuration.baseForegroundColor = [XeniaTheme textPrimary];
  configuration.preferredSymbolConfigurationForImage =
      [UIImageSymbolConfiguration configurationWithPointSize:20.0
                                                      weight:UIImageSymbolWeightSemibold];
  button.configuration = configuration;
  button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption2];
  button.titleLabel.adjustsFontForContentSizeCategory = YES;
  button.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
  button.adjustsImageWhenHighlighted = YES;
  button.imageView.contentMode = UIViewContentModeScaleAspectFit;
  button.accessibilityLabel = accessibility_label;
  button.accessibilityHint = @"Opens in Safari.";
  button.accessibilityTraits |= UIAccessibilityTraitLink;
  button.largeContentTitle = accessibility_label;
  button.showsLargeContentViewer = YES;
  [button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
  [button.widthAnchor constraintEqualToConstant:48.0].active = YES;
  [button.heightAnchor constraintEqualToConstant:44.0].active = YES;
  return button;
}
