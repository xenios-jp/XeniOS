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
  label.adjustsFontForContentSizeCategory = YES;
}

void xe_apply_monospaced_label_font(UILabel* label, UIFontTextStyle text_style, CGFloat point_size,
                                    UIFontWeight weight) {
  if (!label) {
    return;
  }
  label.font = xe_scaled_monospaced_font(text_style, point_size, weight);
  label.adjustsFontForContentSizeCategory = YES;
}

void xe_apply_button_title_font(UIButton* button, UIFontTextStyle text_style, CGFloat point_size,
                                UIFontWeight weight) {
  if (!button.titleLabel) {
    return;
  }
  button.titleLabel.font = xe_scaled_system_font(text_style, point_size, weight);
  button.titleLabel.adjustsFontForContentSizeCategory = YES;
}

void xe_apply_text_view_font(UITextView* text_view, UIFontTextStyle text_style, CGFloat point_size,
                             UIFontWeight weight, BOOL monospaced) {
  if (!text_view) {
    return;
  }
  text_view.font = monospaced ? xe_scaled_monospaced_font(text_style, point_size, weight)
                              : xe_scaled_system_font(text_style, point_size, weight);
  text_view.adjustsFontForContentSizeCategory = YES;
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

// The same glass "Close" capsule the navigation bars show on every other sheet, for sheets
// that draw their own header.
UIButton* xe_make_ios_sheet_close_button(id target, SEL action) {
  // The round glass ✕ iOS 26 sheets use (UIBarButtonSystemItemClose), for views outside a
  // navigation bar.
  UIButtonConfiguration* config = [UIButtonConfiguration glassButtonConfiguration];
  config.image = [UIImage systemImageNamed:@"xmark"
                         withConfiguration:[UIImageSymbolConfiguration
                                               configurationWithTextStyle:UIFontTextStyleBody
                                                                    scale:UIImageSymbolScaleMedium]];
  config.baseForegroundColor = [XeniaTheme textPrimary];
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.accessibilityLabel = NSLocalizedString(@"Close", @"sheet close button");
  [NSLayoutConstraint activateConstraints:@[
    [button.widthAnchor constraintEqualToConstant:44.0],
    [button.heightAnchor constraintEqualToConstant:44.0],
  ]];
  [button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
  return button;
}

static NSMutableDictionary<NSString*, UIImage*>* ProfilePictures() {
  static NSMutableDictionary<NSString*, UIImage*>* pictures = [[NSMutableDictionary alloc] init];
  return pictures;
}

void xe_set_profile_picture(NSString* gamertag, NSData* png) {
  if (!gamertag.length) {
    return;
  }
  UIImage* image = png.length ? [UIImage imageWithData:png] : nil;
  NSMutableDictionary<NSString*, UIImage*>* pictures = ProfilePictures();
  @synchronized(pictures) {
    if (image) {
      pictures[gamertag.lowercaseString] = image;
    } else {
      [pictures removeObjectForKey:gamertag.lowercaseString];
    }
  }
}

static UIImage* ProfilePicture(NSString* gamertag) {
  if (!gamertag.length) {
    return nil;
  }
  NSMutableDictionary<NSString*, UIImage*>* pictures = ProfilePictures();
  @synchronized(pictures) {
    return [[pictures[gamertag.lowercaseString] retain] autorelease];
  }
}

BOOL xe_profile_has_picture(NSString* gamertag) { return ProfilePicture(gamertag) != nil; }

UIImage* xe_profile_avatar_image(NSString* gamertag, BOOL signed_in, CGFloat diameter) {
  const CGSize size = CGSizeMake(diameter, diameter);
  if (UIImage* picture = ProfilePicture(gamertag)) {
    // Gamer pictures are square tiles; round the corners as the dashboard does.
    UIGraphicsImageRenderer* renderer =
        [[[UIGraphicsImageRenderer alloc] initWithSize:size] autorelease];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext* __unused context) {
      const CGRect rect = CGRectMake(0, 0, size.width, size.height);
      [[UIBezierPath bezierPathWithRoundedRect:rect cornerRadius:diameter * 0.2] addClip];
      [picture drawInRect:rect blendMode:kCGBlendModeNormal alpha:signed_in ? 1.0 : 0.6];
    }];
  }
  NSMutableString* initials = [NSMutableString string];
  for (NSString* word in [gamertag componentsSeparatedByCharactersInSet:
                                       [NSCharacterSet whitespaceCharacterSet]]) {
    if (word.length && initials.length < 2) {
      [initials appendString:[[word substringToIndex:1] uppercaseString]];
    }
  }
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:size] autorelease];
  return [renderer imageWithActions:^(UIGraphicsImageRendererContext* __unused context) {
    UIColor* fill = signed_in ? [XeniaTheme accent] : [UIColor colorWithWhite:0.25 alpha:1.0];
    [fill setFill];
    [[UIBezierPath bezierPathWithOvalInRect:CGRectMake(0, 0, size.width, size.height)] fill];
    NSDictionary* attributes = @{
      NSFontAttributeName : [UIFont systemFontOfSize:diameter * 0.38 weight:UIFontWeightBold],
      NSForegroundColorAttributeName : signed_in ? [XeniaTheme accentFg] : [UIColor whiteColor],
    };
    const CGSize text = [initials sizeWithAttributes:attributes];
    [initials drawAtPoint:CGPointMake((size.width - text.width) / 2.0,
                                      (size.height - text.height) / 2.0)
           withAttributes:attributes];
  }];
}
