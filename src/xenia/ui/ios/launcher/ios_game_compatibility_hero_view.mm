/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_compatibility_hero_view.h"

#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

@implementation XeniaGameCompatibilityHeroView {
  uint32_t title_id_;
  NSString* game_title_;
  NSDictionary* compat_info_;
  NSDictionary* summary_source_;
  UIImage* hero_artwork_;
  UIImage* hero_background_artwork_;
  CAGradientLayer* hero_background_gradient_layer_;
  CAGradientLayer* hero_wave_layer_a_;
  CAGradientLayer* hero_wave_layer_b_;
  CAGradientLayer* hero_wave_layer_c_;
  UIColor* hero_glow_color_;
  UIColor* hero_glow_secondary_color_;
  UIView* hero_handle_view_;
  UILabel* hero_sheet_title_label_;
  UIButton* hero_close_button_;
  UIStackView* hero_content_stack_;
  UIView* hero_header_view_;
  UIView* hero_background_view_;
  UIView* hero_header_card_view_;
  UIImageView* hero_header_backdrop_view_;
  UIVisualEffectView* hero_header_blur_view_;
  CAGradientLayer* hero_header_scrim_layer_;
  UILabel* hero_title_label_;
  UILabel* hero_tid_label_;
  UIStackView* hero_pills_stack_;
  XeniaPaddedLabel* hero_status_pill_;
  XeniaPaddedLabel* hero_perf_pill_;
  UILabel* hero_updated_label_;
  id close_target_;
  SEL close_action_;
  BOOL hero_scroll_layout_initialized_;
}

- (instancetype)initWithTitleID:(uint32_t)title_id
                          title:(NSString*)title
                    closeTarget:(id)close_target
                    closeAction:(SEL)close_action {
  self = [super init];
  if (self) {
    title_id_ = title_id;
    game_title_ = [title copy];
    close_target_ = close_target;
    close_action_ = close_action;
    hero_glow_color_ = [[XeniaTheme accent] retain];
    hero_glow_secondary_color_ = [[XeniaTheme accentHover] retain];
  }
  return self;
}

- (void)dealloc {
  [self hideAndRemoveFromSuperview];
  [game_title_ release];
  [compat_info_ release];
  [summary_source_ release];
  [hero_artwork_ release];
  [hero_background_artwork_ release];
  [hero_background_gradient_layer_ release];
  [hero_wave_layer_a_ release];
  [hero_wave_layer_b_ release];
  [hero_wave_layer_c_ release];
  [hero_glow_color_ release];
  [hero_glow_secondary_color_ release];
  [hero_handle_view_ release];
  [hero_sheet_title_label_ release];
  [hero_close_button_ release];
  [hero_content_stack_ release];
  [hero_header_view_ release];
  [hero_background_view_ release];
  [hero_header_card_view_ release];
  [hero_header_backdrop_view_ release];
  [hero_header_blur_view_ release];
  [hero_header_scrim_layer_ release];
  [hero_title_label_ release];
  [hero_tid_label_ release];
  [hero_pills_stack_ release];
  [hero_status_pill_ release];
  [hero_perf_pill_ release];
  [hero_updated_label_ release];
  [super dealloc];
}

- (UIView*)backgroundView {
  return hero_background_view_;
}

- (UIImage*)heroArtwork {
  return hero_artwork_;
}

- (UIImage*)heroBackgroundArtwork {
  return hero_background_artwork_;
}

- (void)setHeroArtwork:(UIImage*)image {
  if (hero_artwork_ == image) {
    return;
  }
  [hero_artwork_ release];
  hero_artwork_ = [image retain];
  if (!hero_background_artwork_) {
    [self updateHeroGlowColorFromImage:image];
  }
  [self updateHeroHeaderArtwork];
}

- (void)setHeroBackgroundArtwork:(UIImage*)image {
  if (hero_background_artwork_ == image) {
    return;
  }
  [hero_background_artwork_ release];
  hero_background_artwork_ = [image retain];
  [self updateHeroGlowColorFromImage:image];
  [self updateHeroHeaderArtwork];
}

- (void)setCompatInfo:(NSDictionary*)compat_info summarySource:(NSDictionary*)summary_source {
  if (compat_info_ != compat_info) {
    [compat_info_ release];
    compat_info_ = [compat_info retain];
  }
  if (summary_source_ != summary_source) {
    [summary_source_ release];
    summary_source_ = [summary_source retain];
  }
  [self updateHeroHeaderContent];
}

- (NSDictionary*)bestResultSource {
  return summary_source_ ?: compat_info_;
}

- (void)updateHeroHeaderArtwork {
  if (!hero_header_backdrop_view_) {
    return;
  }
  UIImage* display = hero_background_artwork_ ?: hero_artwork_;
  hero_header_backdrop_view_.image = display;
  hero_header_backdrop_view_.hidden = (display == nil);
  hero_header_backdrop_view_.layer.contentsRect =
      hero_background_artwork_ ? CGRectMake(0.0, 0.0, 1.0, 1.0) : CGRectMake(0.0, 0.18, 1.0, 0.82);
  hero_header_card_view_.backgroundColor =
      display ? [XeniaTheme bgSurface] : [XeniaTheme bgSurface2];
  [self applyHeroGlowColors];
}

- (void)updateHeroGlowColorFromImage:(UIImage*)image {
  if (!image) {
    return;
  }
  XEHeroGlowPalette palette = xe_extract_hero_glow_palette(image);
  UIColor* primary = palette.primary ?: [XeniaTheme accent];
  UIColor* secondary = palette.secondary ?: [XeniaTheme accentHover];
  [hero_glow_color_ release];
  hero_glow_color_ = [primary retain];
  [hero_glow_secondary_color_ release];
  hero_glow_secondary_color_ = [secondary retain];
  [self applyHeroGlowColors];
}

- (void)applyHeroGlowColors {
  if (!hero_background_gradient_layer_) {
    return;
  }

  UIColor* primary = hero_glow_color_ ?: [XeniaTheme accent];
  UIColor* secondary =
      hero_glow_secondary_color_ ?: xe_blend_rgb_colors(primary, [XeniaTheme accentHover], 0.26);
  UIColor* tertiary = xe_blend_rgb_colors(primary, secondary, 0.34);
  CGFloat avg_luma = xe_color_luma(primary) * 0.70 + xe_color_luma(secondary) * 0.30;
  BOOL has_background_art = (hero_background_artwork_ != nil);
  if (hero_header_backdrop_view_) {
    if (has_background_art) {
      hero_header_backdrop_view_.alpha = MIN(0.96, MAX(0.78, 0.94 - avg_luma * 0.20));
    } else {
      hero_header_backdrop_view_.alpha = MIN(0.88, MAX(0.62, 0.82 - avg_luma * 0.24));
    }
  }
  if (hero_header_blur_view_) {
    if (has_background_art) {
      hero_header_blur_view_.alpha = MIN(0.36, MAX(0.14, 0.14 + avg_luma * 0.14));
    } else {
      hero_header_blur_view_.alpha = MIN(0.60, MAX(0.30, 0.36 + avg_luma * 0.18));
    }
  }
  if (hero_header_scrim_layer_) {
    CGFloat top_alpha = MIN(0.30, MAX(0.10, 0.10 + avg_luma * 0.16));
    CGFloat mid_alpha = MIN(0.60, MAX(0.38, 0.38 + avg_luma * 0.20));
    CGFloat bottom_alpha = MIN(0.92, MAX(0.80, 0.80 + avg_luma * 0.12));
    hero_header_scrim_layer_.colors = @[
      (id)[UIColor colorWithWhite:0.0 alpha:top_alpha].CGColor,
      (id)[UIColor colorWithWhite:0.0 alpha:mid_alpha].CGColor,
      (id)[UIColor colorWithWhite:0.0 alpha:bottom_alpha].CGColor,
    ];
    hero_header_scrim_layer_.locations = @[ @0.0, @0.58, @1.0 ];
  }

  hero_background_gradient_layer_.colors = @[
    (id)[primary colorWithAlphaComponent:0.28].CGColor,
    (id)[secondary colorWithAlphaComponent:0.16].CGColor,
    (id)[tertiary colorWithAlphaComponent:0.08].CGColor,
    (id)[UIColor clearColor].CGColor,
  ];
  hero_background_gradient_layer_.locations = @[ @0.0, @0.30, @0.66, @1.0 ];

  if (hero_wave_layer_a_) {
    hero_wave_layer_a_.colors = @[
      (id)[primary colorWithAlphaComponent:0.24].CGColor,
      (id)[primary colorWithAlphaComponent:0.14].CGColor,
      (id)[primary colorWithAlphaComponent:0.06].CGColor,
      (id)[UIColor clearColor].CGColor,
    ];
  }
  if (hero_wave_layer_b_) {
    hero_wave_layer_b_.colors = @[
      (id)[secondary colorWithAlphaComponent:0.21].CGColor,
      (id)[secondary colorWithAlphaComponent:0.12].CGColor,
      (id)[secondary colorWithAlphaComponent:0.05].CGColor,
      (id)[UIColor clearColor].CGColor,
    ];
  }
  if (hero_wave_layer_c_) {
    UIColor* cloud = xe_blend_rgb_colors(secondary, primary, 0.24);
    hero_wave_layer_c_.colors = @[
      (id)[cloud colorWithAlphaComponent:0.18].CGColor,
      (id)[cloud colorWithAlphaComponent:0.10].CGColor,
      (id)[cloud colorWithAlphaComponent:0.04].CGColor,
      (id)[UIColor clearColor].CGColor,
    ];
  }
}

- (void)layoutOverlayFrames {
  if (!hero_header_card_view_) {
    return;
  }
  CGRect bounds = hero_header_card_view_.bounds;
  if (CGRectIsEmpty(bounds)) {
    return;
  }
  CGFloat card_w = bounds.size.width;

  // Handle: centered, 60x6, 8pt from top.
  CGFloat handle_w = 60.0, handle_h = 6.0;
  hero_handle_view_.frame = CGRectMake(floor((card_w - handle_w) / 2.0), 8.0, handle_w, handle_h);

  // Sheet title: centered, below handle.
  CGFloat title_y = CGRectGetMaxY(hero_handle_view_.frame) + 16.0;
  CGFloat title_max_w = card_w - 72.0 - 60.0;  // left margin + close button area
  CGSize title_size = [hero_sheet_title_label_ sizeThatFits:CGSizeMake(title_max_w, CGFLOAT_MAX)];
  hero_sheet_title_label_.frame = CGRectMake(floor((card_w - title_size.width) / 2.0), title_y,
                                             ceil(title_size.width), ceil(title_size.height));

  // Close button: 48x48, right-aligned, vertically centered with title.
  CGFloat btn_size = 48.0;
  CGFloat title_center_y = CGRectGetMidY(hero_sheet_title_label_.frame);
  hero_close_button_.frame = CGRectMake(card_w - 16.0 - btn_size,
                                        floor(title_center_y - btn_size / 2.0), btn_size, btn_size);
}

- (void)updateGradientFrames {
  if (!hero_header_card_view_) {
    return;
  }
  CGRect bounds = hero_header_card_view_.bounds;
  if (CGRectIsEmpty(bounds)) {
    return;
  }
  hero_header_scrim_layer_.frame = bounds;
  hero_background_gradient_layer_.frame = bounds;
  CGRect wave_frame = CGRectInset(bounds, -bounds.size.width * 0.44, -bounds.size.height * 0.86);
  wave_frame.origin.y -= bounds.size.height * 0.60;
  hero_wave_layer_a_.frame = wave_frame;
  hero_wave_layer_b_.frame = wave_frame;
  hero_wave_layer_c_.frame = wave_frame;
}

- (void)ensureTopGlowAnimation {
  if (!hero_wave_layer_a_ || !hero_wave_layer_b_ || !hero_wave_layer_c_ ||
      UIAccessibilityIsReduceMotionEnabled()) {
    [hero_wave_layer_a_ removeAllAnimations];
    [hero_wave_layer_b_ removeAllAnimations];
    [hero_wave_layer_c_ removeAllAnimations];
    hero_wave_layer_a_.opacity = 0.08;
    hero_wave_layer_b_.opacity = 0.06;
    hero_wave_layer_c_.opacity = 0.05;
    return;
  }

  NSArray<CAGradientLayer*>* waves =
      @[ hero_wave_layer_a_, hero_wave_layer_b_, hero_wave_layer_c_ ];
  NSArray<NSNumber*>* durations = @[ @19.8, @24.2, @28.4 ];
  NSArray<NSNumber*>* peaks = @[ @0.13, @0.10, @0.08 ];
  NSArray<NSNumber*>* scale_y_to = @[ @1.08, @1.10, @1.07 ];
  NSArray<NSNumber*>* scale_x_to = @[ @1.06, @1.08, @1.05 ];

  for (NSUInteger i = 0; i < waves.count; ++i) {
    CAGradientLayer* wave = waves[i];
    NSString* key = [NSString stringWithFormat:@"xenia.hero.wave.%lu.group", (unsigned long)i];
    if ([wave animationForKey:key]) {
      continue;
    }

    double peak = [peaks[i] doubleValue];
    CAKeyframeAnimation* opacity = [CAKeyframeAnimation animationWithKeyPath:@"opacity"];
    opacity.values =
        @[ @0.02, @(peak * 0.70), @(peak * 0.95), @(peak * 0.50), @0.03, @(peak * 0.82), @0.02 ];
    opacity.keyTimes = @[ @0.0, @0.15, @0.33, @0.51, @0.68, @0.86, @1.0 ];
    opacity.calculationMode = kCAAnimationLinear;

    CAKeyframeAnimation* scale_y = [CAKeyframeAnimation animationWithKeyPath:@"transform.scale.y"];
    scale_y.values = @[ @0.97, @1.01, scale_y_to[i], @1.02, @0.99, @1.03, @0.97 ];
    scale_y.keyTimes = @[ @0.0, @0.18, @0.36, @0.55, @0.71, @0.88, @1.0 ];
    scale_y.calculationMode = kCAAnimationLinear;

    CAKeyframeAnimation* scale_x = [CAKeyframeAnimation animationWithKeyPath:@"transform.scale.x"];
    scale_x.values = @[ @0.98, @1.01, scale_x_to[i], @1.01, @0.99, @1.02, @0.98 ];
    scale_x.keyTimes = @[ @0.0, @0.16, @0.34, @0.52, @0.69, @0.87, @1.0 ];
    scale_x.calculationMode = kCAAnimationLinear;

    CGFloat x_span = wave.bounds.size.width * (0.004 + 0.002 * (CGFloat)i);
    CGFloat y_span = wave.bounds.size.height * (0.003 + 0.002 * (CGFloat)i);
    CGFloat x_jitter = (((CGFloat)arc4random_uniform(180) / 100.0) - 0.90) * x_span;
    CGFloat y_jitter = (((CGFloat)arc4random_uniform(180) / 100.0) - 0.90) * y_span;
    CAKeyframeAnimation* drift_x =
        [CAKeyframeAnimation animationWithKeyPath:@"transform.translation.x"];
    drift_x.values = @[
      @(-x_span * 0.26), @(x_span * 0.18 + x_jitter), @(x_span * 0.34), @(x_span * -0.08),
      @(-x_span * 0.22)
    ];
    drift_x.keyTimes = @[ @0.0, @0.27, @0.52, @0.78, @1.0 ];
    drift_x.calculationMode = kCAAnimationLinear;
    CAKeyframeAnimation* drift_y =
        [CAKeyframeAnimation animationWithKeyPath:@"transform.translation.y"];
    drift_y.values = @[
      @(-y_span * 0.22), @(y_span * 0.20), @(y_span * 0.34 + y_jitter), @(y_span * -0.04),
      @(-y_span * 0.22)
    ];
    drift_y.keyTimes = @[ @0.0, @0.29, @0.58, @0.81, @1.0 ];
    drift_y.calculationMode = kCAAnimationLinear;

    CAAnimationGroup* group = [CAAnimationGroup animation];
    group.animations = @[ opacity, scale_y, scale_x, drift_x, drift_y ];
    group.duration = [durations[i] doubleValue] + ((double)arc4random_uniform(520) / 100.0);
    group.beginTime = CACurrentMediaTime() + ((double)arc4random_uniform(420) / 100.0);
    group.repeatCount = HUGE_VALF;
    group.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionLinear];
    group.removedOnCompletion = NO;
    [wave addAnimation:group forKey:key];
  }
}

- (void)updateHeroHeaderContent {
  if (!hero_header_view_) {
    return;
  }

  hero_title_label_.text = game_title_.length > 0 ? game_title_ : @"Unknown Title";
  hero_tid_label_.text = title_id_ ? [NSString stringWithFormat:@"Title ID: %08X", title_id_]
                                   : @"No title ID available";

  NSDictionary* summary_source = [self bestResultSource];
  NSString* status = xe_string_from_object(summary_source[@"status"]);
  if (status.length > 0) {
    UIColor* status_color = xe_compat_status_color(status);
    hero_status_pill_.text = xe_compat_status_label(status);
    hero_status_pill_.textColor = status_color;
    hero_status_pill_.backgroundColor = [status_color colorWithAlphaComponent:0.1];
    hero_status_pill_.hidden = NO;
  } else {
    hero_status_pill_.hidden = YES;
  }

  NSString* perf = xe_string_from_object(summary_source[@"perf"]);
  if (perf.length > 0) {
    UIColor* perf_color = xe_compat_perf_color(perf);
    hero_perf_pill_.text = xe_compat_perf_label(perf);
    hero_perf_pill_.textColor = perf_color;
    hero_perf_pill_.backgroundColor = [perf_color colorWithAlphaComponent:0.1];
    hero_perf_pill_.hidden = NO;
  } else {
    hero_perf_pill_.hidden = YES;
  }
  hero_pills_stack_.hidden = hero_status_pill_.hidden && hero_perf_pill_.hidden;

  NSString* updated_at =
      [compat_info_[@"updatedAt"] isKindOfClass:[NSString class]]
          ? compat_info_[@"updatedAt"]
          : ([summary_source[@"date"] isKindOfClass:[NSString class]] ? summary_source[@"date"]
                                                                      : nil);
  if (updated_at.length > 0) {
    hero_updated_label_.text =
        [NSString stringWithFormat:@"Last updated: %@", xe_format_iso_date(updated_at)];
    hero_updated_label_.hidden = NO;
  } else {
    hero_updated_label_.hidden = YES;
  }

  [self updateHeroHeaderArtwork];
}

- (void)layoutInTableView:(UITableView*)table_view
          controllerView:(UIView*)controller_view
                hostView:(UIView*)host_view {
  if (!hero_header_view_) {
    return;
  }
  CGFloat width = CGRectGetWidth(table_view.bounds);
  if (width <= 0.0) {
    width = CGRectGetWidth(controller_view.bounds);
  }
  if (width <= 0.0) {
    return;
  }

  CGFloat height = xe_compat_hero_height_for_width(width, hero_background_artwork_);
  if (!hero_background_view_) {
    hero_background_view_ = [[UIView alloc] initWithFrame:CGRectZero];
    hero_background_view_.backgroundColor = [XeniaTheme bgPrimary];
    hero_background_view_.clipsToBounds = NO;
    [hero_background_view_ addSubview:hero_header_view_];
  }
  host_view = host_view ?: controller_view.superview ?: controller_view;
  CGFloat safe_top = 0.0;
  if (@available(iOS 11.0, *)) {
    UIWindow* window = host_view.window ?: controller_view.window;
    safe_top = window ? window.safeAreaInsets.top : host_view.safeAreaInsets.top;
  }
  if (host_view && hero_background_view_.superview != host_view) {
    [hero_background_view_ removeFromSuperview];
    [host_view addSubview:hero_background_view_];
  }
  CGRect table_frame = host_view ? [controller_view convertRect:controller_view.bounds
                                                         toView:host_view]
                                 : controller_view.bounds;
  CGFloat hero_top = CGRectGetMinY(table_frame);
  hero_background_view_.frame =
      CGRectMake(CGRectGetMinX(table_frame), hero_top, width, height + safe_top);
  hero_header_view_.frame = CGRectMake(0.0, safe_top, width, height);
  CGFloat desired_top_inset =
      CGRectGetMaxY(hero_background_view_.frame) - CGRectGetMinY(table_frame) + 12.0;
  CGFloat relative_offset = table_view.contentOffset.y + table_view.contentInset.top;
  if (fabs(table_view.contentInset.top - desired_top_inset) > 0.5) {
    UIEdgeInsets content_inset = table_view.contentInset;
    content_inset.top = desired_top_inset;
    table_view.contentInset = content_inset;
    if (@available(iOS 13.0, *)) {
      UIEdgeInsets vertical_insets = table_view.verticalScrollIndicatorInsets;
      vertical_insets.top = desired_top_inset;
      table_view.verticalScrollIndicatorInsets = vertical_insets;
    } else {
      UIEdgeInsets indicator_insets = content_inset;
      indicator_insets.top = desired_top_inset;
      table_view.scrollIndicatorInsets = indicator_insets;
    }
    table_view.contentOffset = CGPointMake(table_view.contentOffset.x,
                                           relative_offset - desired_top_inset);
  }
  if (!hero_scroll_layout_initialized_) {
    table_view.contentOffset = CGPointMake(table_view.contentOffset.x, -desired_top_inset);
    hero_scroll_layout_initialized_ = YES;
  }
  [hero_header_view_ setNeedsLayout];
  [hero_header_view_ layoutIfNeeded];
  hero_header_scrim_layer_.frame = hero_header_card_view_.bounds;
  [self updateHeroHeaderArtwork];
}

- (void)buildIfNeededWithTableView:(UITableView*)table_view
                     controllerView:(UIView*)controller_view {
  if (hero_header_view_) {
    [self updateHeroHeaderContent];
    [self layoutInTableView:table_view controllerView:controller_view hostView:nil];
    return;
  }

  CGFloat width = CGRectGetWidth(table_view.bounds);
  if (width <= 0.0) {
    width = CGRectGetWidth(controller_view.bounds);
  }
  if (width <= 0.0) {
    width = UIScreen.mainScreen.bounds.size.width;
  }

  CGFloat height = xe_compat_hero_height_for_width(width, hero_background_artwork_);
  hero_header_view_ = [[UIView alloc] initWithFrame:CGRectMake(0.0, 0.0, width, height)];
  hero_header_view_.backgroundColor = [UIColor clearColor];
  // The hero card composites text on top of the game cover artwork (always
  // a dark image for Xbox 360 box art). Force-dark on this subtree so
  // textPrimary / textSecondary and the blur style stay light-on-dark
  // regardless of the system mode — same approach as the in-game HUD,
  // which is also rendered over a darker canvas.
  hero_header_view_.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  hero_background_view_ = [[UIView alloc] initWithFrame:hero_header_view_.frame];
  hero_background_view_.backgroundColor = [XeniaTheme bgPrimary];
  hero_background_view_.clipsToBounds = NO;
  [hero_background_view_ addSubview:hero_header_view_];

  hero_header_card_view_ = [[UIView alloc] init];
  hero_header_card_view_.translatesAutoresizingMaskIntoConstraints = NO;
  hero_header_card_view_.backgroundColor = [XeniaTheme bgSurface];
  // Intentionally larger than the design system's XeniaRadiusXxl (24pt). The
  // hero card is the largest sheet surface and reads as "iPad-class chrome"
  // at 28pt; smaller values make it feel cramped against the artwork edge.
  hero_header_card_view_.layer.cornerRadius = 28.0;
  if (@available(iOS 11.0, *)) {
    hero_header_card_view_.layer.maskedCorners = kCALayerMinXMinYCorner | kCALayerMaxXMinYCorner;
  }
  hero_header_card_view_.layer.borderWidth = 0.5;
  hero_header_card_view_.layer.borderColor = [XeniaTheme border].CGColor;
  hero_header_card_view_.clipsToBounds = YES;
  [hero_header_view_ addSubview:hero_header_card_view_];

  hero_header_backdrop_view_ = [[UIImageView alloc] init];
  hero_header_backdrop_view_.translatesAutoresizingMaskIntoConstraints = NO;
  hero_header_backdrop_view_.contentMode = UIViewContentModeScaleAspectFill;
  hero_header_backdrop_view_.clipsToBounds = YES;
  [hero_header_card_view_ addSubview:hero_header_backdrop_view_];

  // Clear-variant liquid glass on iOS 26+ (UIGlassEffectStyleClear, per
  // Apple HIG for chrome sitting over visually rich content like the game
  // artwork backdrop). On iOS 18-25 the helper falls back to
  // UIBlurEffectStyleSystemUltraThinMaterial — the same backdrop this view
  // used before the helper existed, so pre-iOS-26 rendering is unchanged.
  // The parent's force-dark override resolves the blur to a dark variant
  // even when the rest of the app is light.
  hero_header_blur_view_ = [[UIVisualEffectView alloc]
      initWithEffect:xe_make_chrome_visual_effect(YES)];
  hero_header_blur_view_.translatesAutoresizingMaskIntoConstraints = NO;
  [hero_header_card_view_ addSubview:hero_header_blur_view_];

  hero_header_scrim_layer_ = [[CAGradientLayer layer] retain];
  [hero_header_card_view_.layer addSublayer:hero_header_scrim_layer_];

  // Base glow gradient from the top.
  hero_background_gradient_layer_ = [[CAGradientLayer layer] retain];
  [hero_header_card_view_.layer insertSublayer:hero_background_gradient_layer_
                                         above:hero_header_scrim_layer_];

  // Three layered radial-glow waves.
  hero_wave_layer_a_ = [[CAGradientLayer layer] retain];
  hero_wave_layer_a_.type = kCAGradientLayerRadial;
  hero_wave_layer_a_.locations = @[ @0.00, @0.34, @0.68, @1.00 ];
  hero_wave_layer_a_.startPoint = CGPointMake(0.52, -0.44);
  hero_wave_layer_a_.endPoint = CGPointMake(0.52, 1.00);
  hero_wave_layer_a_.opacity = 0.06;

  hero_wave_layer_b_ = [[CAGradientLayer layer] retain];
  hero_wave_layer_b_.type = kCAGradientLayerRadial;
  hero_wave_layer_b_.locations = @[ @0.00, @0.36, @0.70, @1.00 ];
  hero_wave_layer_b_.startPoint = CGPointMake(0.34, -0.52);
  hero_wave_layer_b_.endPoint = CGPointMake(0.36, 1.00);
  hero_wave_layer_b_.opacity = 0.05;

  hero_wave_layer_c_ = [[CAGradientLayer layer] retain];
  hero_wave_layer_c_.type = kCAGradientLayerRadial;
  hero_wave_layer_c_.locations = @[ @0.00, @0.40, @0.72, @1.00 ];
  hero_wave_layer_c_.startPoint = CGPointMake(0.70, -0.50);
  hero_wave_layer_c_.endPoint = CGPointMake(0.68, 1.00);
  hero_wave_layer_c_.opacity = 0.04;

  [hero_header_card_view_.layer insertSublayer:hero_wave_layer_a_
                                         above:hero_background_gradient_layer_];
  [hero_header_card_view_.layer insertSublayer:hero_wave_layer_b_ above:hero_wave_layer_a_];
  [hero_header_card_view_.layer insertSublayer:hero_wave_layer_c_ above:hero_wave_layer_b_];

  [self applyHeroGlowColors];

  // Handle, sheet title, and close button use manual frames (not auto
  // layout) so they survive hero_background_view_ remove/re-add cycles
  // across navigation push/pop transitions without constraint breakage.
  hero_handle_view_ = [[UIView alloc] init];
  // Dynamic so the handle reads as a light bar in dark mode and a dark bar in
  // light mode. Plain whiteColor would vanish on white surfaces.
  hero_handle_view_.backgroundColor =
      [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
        return (traits.userInterfaceStyle == UIUserInterfaceStyleLight)
                   ? [[UIColor blackColor] colorWithAlphaComponent:0.34]
                   : [[UIColor whiteColor] colorWithAlphaComponent:0.34];
      }];
  hero_handle_view_.layer.cornerRadius = 3.0;  // handle_h / 2 — capsule grabber.
  [hero_header_card_view_ addSubview:hero_handle_view_];

  hero_sheet_title_label_ = [[UILabel alloc] init];
  hero_sheet_title_label_.text = @"Compatibility";
  hero_sheet_title_label_.textColor = [XeniaTheme textPrimary];
  hero_sheet_title_label_.textAlignment = NSTextAlignmentCenter;
  xe_apply_label_font(hero_sheet_title_label_, UIFontTextStyleTitle2, 18.0, UIFontWeightSemibold);
  [hero_header_card_view_ addSubview:hero_sheet_title_label_];

  hero_close_button_ = [xe_make_ios_sheet_close_button(close_target_, close_action_) retain];
  // Convert close button from auto layout to manual frame positioning.
  NSArray<NSLayoutConstraint*>* close_button_constraints =
      [[hero_close_button_.constraints copy] autorelease];
  for (NSLayoutConstraint* c in close_button_constraints) {
    c.active = NO;
  }
  hero_close_button_.translatesAutoresizingMaskIntoConstraints = YES;
  [hero_header_card_view_ addSubview:hero_close_button_];

  hero_content_stack_ = [[UIStackView alloc] init];
  hero_content_stack_.translatesAutoresizingMaskIntoConstraints = NO;
  hero_content_stack_.axis = UILayoutConstraintAxisVertical;
  hero_content_stack_.spacing = 8.0;
  [hero_header_card_view_ addSubview:hero_content_stack_];

  hero_title_label_ = [[UILabel alloc] init];
  hero_title_label_.translatesAutoresizingMaskIntoConstraints = NO;
  hero_title_label_.textColor = [XeniaTheme textPrimary];
  hero_title_label_.numberOfLines = 2;
  hero_title_label_.lineBreakMode = NSLineBreakByTruncatingTail;
  xe_apply_label_font(hero_title_label_, UIFontTextStyleLargeTitle, 26.0, UIFontWeightBold);
  [hero_content_stack_ addArrangedSubview:hero_title_label_];

  hero_tid_label_ = [[UILabel alloc] init];
  hero_tid_label_.translatesAutoresizingMaskIntoConstraints = NO;
  hero_tid_label_.textColor = [XeniaTheme textSecondary];
  xe_apply_monospaced_label_font(hero_tid_label_, UIFontTextStyleBody, 13.0, UIFontWeightRegular);
  [hero_content_stack_ addArrangedSubview:hero_tid_label_];

  hero_pills_stack_ = [[UIStackView alloc] init];
  hero_pills_stack_.translatesAutoresizingMaskIntoConstraints = NO;
  hero_pills_stack_.axis = UILayoutConstraintAxisHorizontal;
  hero_pills_stack_.spacing = 10.0;
  hero_pills_stack_.alignment = UIStackViewAlignmentCenter;
  [hero_pills_stack_ setContentHuggingPriority:UILayoutPriorityRequired
                                       forAxis:UILayoutConstraintAxisHorizontal];
  [hero_pills_stack_ setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                     forAxis:UILayoutConstraintAxisHorizontal];

  hero_status_pill_ = [[XeniaPaddedLabel alloc] init];
  hero_status_pill_.translatesAutoresizingMaskIntoConstraints = NO;
  hero_status_pill_.padding = UIEdgeInsetsMake(2, 7, 2, 7);
  hero_status_pill_.textAlignment = NSTextAlignmentCenter;
  hero_status_pill_.layer.cornerRadius = XeniaRadiusMd;
  hero_status_pill_.clipsToBounds = YES;
  xe_apply_label_font(hero_status_pill_, UIFontTextStyleCaption1, 11.0, UIFontWeightMedium);
  [hero_status_pill_ setContentHuggingPriority:UILayoutPriorityRequired
                                       forAxis:UILayoutConstraintAxisHorizontal];
  [hero_status_pill_ setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                     forAxis:UILayoutConstraintAxisHorizontal];
  hero_status_pill_.hidden = YES;

  hero_perf_pill_ = [[XeniaPaddedLabel alloc] init];
  hero_perf_pill_.translatesAutoresizingMaskIntoConstraints = NO;
  hero_perf_pill_.padding = UIEdgeInsetsMake(2, 7, 2, 7);
  hero_perf_pill_.textAlignment = NSTextAlignmentCenter;
  hero_perf_pill_.layer.cornerRadius = XeniaRadiusMd;
  hero_perf_pill_.clipsToBounds = YES;
  xe_apply_label_font(hero_perf_pill_, UIFontTextStyleCaption1, 11.0, UIFontWeightMedium);
  [hero_perf_pill_ setContentHuggingPriority:UILayoutPriorityRequired
                                     forAxis:UILayoutConstraintAxisHorizontal];
  [hero_perf_pill_ setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                   forAxis:UILayoutConstraintAxisHorizontal];
  hero_perf_pill_.hidden = YES;

  [hero_pills_stack_ addArrangedSubview:hero_status_pill_];
  [hero_pills_stack_ addArrangedSubview:hero_perf_pill_];

  UIView* pills_row = [[[UIView alloc] init] autorelease];
  pills_row.translatesAutoresizingMaskIntoConstraints = NO;
  [pills_row addSubview:hero_pills_stack_];
  [NSLayoutConstraint activateConstraints:@[
    [hero_pills_stack_.topAnchor constraintEqualToAnchor:pills_row.topAnchor],
    [hero_pills_stack_.leadingAnchor constraintEqualToAnchor:pills_row.leadingAnchor],
    [hero_pills_stack_.bottomAnchor constraintEqualToAnchor:pills_row.bottomAnchor],
    [hero_pills_stack_.trailingAnchor constraintLessThanOrEqualToAnchor:pills_row.trailingAnchor],
  ]];
  [hero_content_stack_ addArrangedSubview:pills_row];

  hero_updated_label_ = [[UILabel alloc] init];
  hero_updated_label_.translatesAutoresizingMaskIntoConstraints = NO;
  hero_updated_label_.textColor = [XeniaTheme textMuted];
  hero_updated_label_.numberOfLines = 1;
  xe_apply_label_font(hero_updated_label_, UIFontTextStyleFootnote, 12.0, UIFontWeightRegular);
  [hero_content_stack_ addArrangedSubview:hero_updated_label_];

  [NSLayoutConstraint activateConstraints:@[
    [hero_header_card_view_.topAnchor constraintEqualToAnchor:hero_header_view_.topAnchor],
    [hero_header_card_view_.leadingAnchor constraintEqualToAnchor:hero_header_view_.leadingAnchor],
    [hero_header_card_view_.trailingAnchor
        constraintEqualToAnchor:hero_header_view_.trailingAnchor],
    [hero_header_card_view_.bottomAnchor constraintEqualToAnchor:hero_header_view_.bottomAnchor],
    [hero_header_backdrop_view_.topAnchor constraintEqualToAnchor:hero_header_card_view_.topAnchor],
    [hero_header_backdrop_view_.leadingAnchor
        constraintEqualToAnchor:hero_header_card_view_.leadingAnchor],
    [hero_header_backdrop_view_.trailingAnchor
        constraintEqualToAnchor:hero_header_card_view_.trailingAnchor],
    [hero_header_backdrop_view_.bottomAnchor
        constraintEqualToAnchor:hero_header_card_view_.bottomAnchor],
    [hero_header_blur_view_.topAnchor constraintEqualToAnchor:hero_header_card_view_.topAnchor],
    [hero_header_blur_view_.leadingAnchor
        constraintEqualToAnchor:hero_header_card_view_.leadingAnchor],
    [hero_header_blur_view_.trailingAnchor
        constraintEqualToAnchor:hero_header_card_view_.trailingAnchor],
    [hero_header_blur_view_.bottomAnchor
        constraintEqualToAnchor:hero_header_card_view_.bottomAnchor],
    // content_stack uses auto layout only for leading/trailing/bottom.
    // No top constraint — its top position is determined by its content height
    // and the bottom anchor, avoiding any conflict with the manual-frame views above.
    [hero_content_stack_.leadingAnchor constraintEqualToAnchor:hero_header_card_view_.leadingAnchor
                                                      constant:28.0],
    [hero_content_stack_.trailingAnchor
        constraintEqualToAnchor:hero_header_card_view_.trailingAnchor
                       constant:-28.0],
    [hero_content_stack_.bottomAnchor constraintEqualToAnchor:hero_header_card_view_.bottomAnchor
                                                     constant:-26.0],
  ]];

  [hero_header_view_ setNeedsLayout];
  [hero_header_view_ layoutIfNeeded];
  [self layoutOverlayFrames];
  [self updateHeroHeaderContent];
  [self layoutInTableView:table_view controllerView:controller_view hostView:nil];
}

- (void)loadArtworkIfNeeded {
  if (!title_id_) {
    return;
  }

  UIImage* cached_background = xe_cached_game_background_art(title_id_);
  if (cached_background) {
    [self setHeroBackgroundArtwork:cached_background];
    [self updateHeroGlowColorFromImage:cached_background];
  }

  if (!hero_artwork_) {
    UIImage* cached_cover = xe_cached_game_art(title_id_);
    if (cached_cover) {
      [self setHeroArtwork:cached_cover];
    }
  }

  // If we have artwork but no background, use cover art for the glow.
  if (!cached_background && hero_artwork_) {
    [self updateHeroGlowColorFromImage:hero_artwork_];
  }

  if ((cached_background || hero_artwork_) && hero_header_view_) {
    [self updateHeroHeaderContent];
  }

  const uint32_t expected_title_id = title_id_;
  if (!cached_background) {
    xe_fetch_game_background_art(expected_title_id, ^(UIImage* image) {
      if (!image || self->title_id_ != expected_title_id) {
        return;
      }
      [self setHeroBackgroundArtwork:image];
      [self updateHeroGlowColorFromImage:image];
      if (self->hero_header_view_) {
        [self updateHeroHeaderContent];
      }
    });
  }

  if (!hero_artwork_) {
    xe_fetch_game_art(expected_title_id, ^(UIImage* image) {
      if (!image || self->title_id_ != expected_title_id) {
        return;
      }
      [self setHeroArtwork:image];
      if (!self->hero_background_artwork_) {
        [self updateHeroGlowColorFromImage:image];
      }
      if (self->hero_header_view_) {
        [self updateHeroHeaderContent];
      }
    });
  }
}


- (void)updateTraitColors {
  if (hero_header_card_view_) {
    hero_header_card_view_.layer.borderColor = [XeniaTheme border].CGColor;
  }
}

- (void)setHidden:(BOOL)hidden {
  hero_background_view_.hidden = hidden;
}

- (void)hideAndRemoveFromSuperview {
  hero_background_view_.hidden = YES;
  [hero_background_view_ removeFromSuperview];
}

@end
