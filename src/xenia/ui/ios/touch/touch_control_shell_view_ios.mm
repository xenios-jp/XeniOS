/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/touch/touch_control_shell_view_ios.h"

#include <algorithm>
#include <cmath>

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"
#include "xenia/ui/ios/touch/touch_overlay_style_ios.h"

namespace {

xe::hid::touch::IOSTouchAnalogOutput EffectiveShellDragOutput(
    const xe::hid::touch::IOSTouchControlDefinition& control) {
  if (control.drag_output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
    return control.drag_output;
  }
  if (control.enables_relative_look) {
    return xe::hid::touch::IOSTouchAnalogOutput::kLook;
  }
  if (control.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
    return control.action == xe::hid::touch::IOSTouchAction::kMove
               ? xe::hid::touch::IOSTouchAnalogOutput::kMove
               : xe::hid::touch::IOSTouchAnalogOutput::kLook;
  }
  return xe::hid::touch::IOSTouchAnalogOutput::kNone;
}

xe::hid::touch::IOSTouchAnalogOutput EffectiveShellBehaviorAnalogOutput(
    const xe::hid::touch::IOSTouchInteractionBehavior& behavior) {
  if (behavior.analog_output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
    return behavior.analog_output;
  }
  return behavior.enables_relative_look ? xe::hid::touch::IOSTouchAnalogOutput::kLook
                                        : xe::hid::touch::IOSTouchAnalogOutput::kNone;
}

NSString* XeniaTouchShortActionName(xe::hid::touch::IOSTouchAction action) {
  using xe::hid::touch::IOSTouchAction;
  switch (action) {
    case IOSTouchAction::kNone:
      return @"Off";
    case IOSTouchAction::kMove:
      return @"Mv";
    case IOSTouchAction::kLook:
      return @"Lk";
    case IOSTouchAction::kPauseMenu:
      return @"P";
    case IOSTouchAction::kButtonA:
      return @"A";
    case IOSTouchAction::kButtonB:
      return @"B";
    case IOSTouchAction::kButtonX:
      return @"X";
    case IOSTouchAction::kButtonY:
      return @"Y";
    case IOSTouchAction::kLeftBumper:
      return @"LB";
    case IOSTouchAction::kRightBumper:
      return @"RB";
    case IOSTouchAction::kLeftTrigger:
      return @"LT";
    case IOSTouchAction::kRightTrigger:
      return @"RT";
    case IOSTouchAction::kBack:
      return @"Bk";
    case IOSTouchAction::kStart:
      return @"St";
    case IOSTouchAction::kLeftThumb:
      return @"LS";
    case IOSTouchAction::kRightThumb:
      return @"RS";
    case IOSTouchAction::kDpadUp:
      return @"DU";
    case IOSTouchAction::kDpadDown:
      return @"DD";
    case IOSTouchAction::kDpadLeft:
      return @"DL";
    case IOSTouchAction::kDpadRight:
      return @"DR";
  }
  return @"Ctrl";
}

NSString* XeniaTouchShortAnalogOutputName(
    xe::hid::touch::IOSTouchAnalogOutput output) {
  using xe::hid::touch::IOSTouchAnalogOutput;
  switch (output) {
    case IOSTouchAnalogOutput::kNone:
      return @"Off";
    case IOSTouchAnalogOutput::kLook:
      return @"Lk";
    case IOSTouchAnalogOutput::kMove:
      return @"Mv";
  }
  return @"Off";
}

NSString* XeniaTouchBehaviorBadgeText(
    const xe::hid::touch::IOSTouchControlDefinition& control) {
  NSMutableArray<NSString*>* chips = [NSMutableArray arrayWithCapacity:3];
  const xe::hid::touch::IOSTouchAnalogOutput primary_output =
      EffectiveShellDragOutput(control);
  if (primary_output != xe::hid::touch::IOSTouchAnalogOutput::kNone &&
      control.type != xe::hid::touch::IOSTouchControlType::kMoveStick) {
    [chips addObject:[NSString stringWithFormat:@"D:%@",
                                                XeniaTouchShortAnalogOutputName(primary_output)]];
  }
  // The D-pad ring is visible on the stick itself; show hidden sprint-style
  // behavior first so the badge explains what users cannot otherwise see.
  const auto& behavior = control.secondary_behavior;
  if (behavior.trigger != xe::hid::touch::IOSTouchInteractionTrigger::kNone) {
    NSString* trigger = @"G";
    switch (behavior.trigger) {
      case xe::hid::touch::IOSTouchInteractionTrigger::kHold:
        trigger = @"H";
        break;
      case xe::hid::touch::IOSTouchInteractionTrigger::kHoldDrag:
        trigger = @"HD";
        break;
      case xe::hid::touch::IOSTouchInteractionTrigger::kDoubleTap:
        trigger = @"2x";
        break;
      case xe::hid::touch::IOSTouchInteractionTrigger::kDoubleTapForward:
        trigger = @"2xF";
        break;
      case xe::hid::touch::IOSTouchInteractionTrigger::kNone:
        trigger = @"";
        break;
    }
    if (behavior.action != xe::hid::touch::IOSTouchAction::kNone) {
      [chips addObject:[NSString stringWithFormat:@"%@:%@", trigger,
                                                  XeniaTouchShortActionName(behavior.action)]];
    }
    const xe::hid::touch::IOSTouchAnalogOutput secondary_output =
        EffectiveShellBehaviorAnalogOutput(behavior);
    if (secondary_output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
      [chips addObject:[NSString stringWithFormat:@"%@:%@", trigger,
                                                  XeniaTouchShortAnalogOutputName(
                                                      secondary_output)]];
    }
  }

  if (control.type == xe::hid::touch::IOSTouchControlType::kMoveStick &&
      control.move_with_dpad_ring) {
    [chips addObject:@"DPad"];
  }

  if (!chips.count) {
    return @"";
  }
  if (chips.count > 1) {
    return [NSString stringWithFormat:@"%@+%lu", [chips objectAtIndex:0],
                                      static_cast<unsigned long>(chips.count - 1)];
  }
  return [chips firstObject];
}

}  // namespace

@implementation XeniaTouchControlShellView {
  UILabel* label_;
  UILabel* behavior_badge_;
  xe::hid::touch::IOSTouchControlDefinition control_;
  BOOL touch_active_;
  BOOL conflict_highlighted_;
  BOOL chrome_suppressed_;
  BOOL behavior_annotations_visible_;
  UIImageView* dpad_arrow_up_;
  UIImageView* dpad_arrow_down_;
  UIImageView* dpad_arrow_left_;
  UIImageView* dpad_arrow_right_;
}

- (instancetype)initWithControl:
    (const xe::hid::touch::IOSTouchControlDefinition&)control {
  if (!(self = [super initWithFrame:CGRectZero])) {
    return nil;
  }

  self.backgroundColor = [UIColor clearColor];
  self.userInteractionEnabled = NO;
  self.layer.borderWidth = 1.5;

  label_ = [[UILabel alloc] initWithFrame:CGRectZero];
  label_.backgroundColor = [UIColor clearColor];
  label_.textAlignment = NSTextAlignmentCenter;
  xe_apply_label_font(label_, UIFontTextStyleCaption1, 12.0,
                      UIFontWeightSemibold);
  label_.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.92];
  label_.adjustsFontSizeToFitWidth = YES;
  label_.minimumScaleFactor = 0.6f;
  [self addSubview:label_];

  behavior_badge_ = [[UILabel alloc] initWithFrame:CGRectZero];
  behavior_badge_.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.78f];
  behavior_badge_.textColor = [UIColor whiteColor];
  behavior_badge_.textAlignment = NSTextAlignmentCenter;
  behavior_badge_.numberOfLines = 1;
  behavior_badge_.adjustsFontSizeToFitWidth = YES;
  behavior_badge_.minimumScaleFactor = 0.45f;
  behavior_badge_.hidden = YES;
  behavior_badge_.userInteractionEnabled = NO;
  behavior_badge_.layer.cornerRadius = 5.0f;
  behavior_badge_.layer.borderWidth = 0.8f;
  behavior_badge_.layer.borderColor =
      [[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.82f].CGColor;
  behavior_badge_.clipsToBounds = YES;
  xe_apply_label_font(behavior_badge_, UIFontTextStyleCaption2, 6.5,
                      UIFontWeightSemibold);
  [self addSubview:behavior_badge_];

  [self applyControlDefinition:control];
  return self;
}

- (void)dealloc {
  [dpad_arrow_right_ release];
  [dpad_arrow_left_ release];
  [dpad_arrow_down_ release];
  [dpad_arrow_up_ release];
  [behavior_badge_ release];
  [label_ release];
  [super dealloc];
}

- (void)ensureDpadArrowViews {
  if (dpad_arrow_up_) {
    return;
  }
  UIImageSymbolConfiguration* arrow_config =
      [UIImageSymbolConfiguration configurationWithPointSize:18.0
                                                      weight:UIImageSymbolWeightBold];
  UIImage* up_image =
      [UIImage systemImageNamed:@"chevron.up" withConfiguration:arrow_config];
  UIImage* down_image =
      [UIImage systemImageNamed:@"chevron.down" withConfiguration:arrow_config];
  UIImage* left_image =
      [UIImage systemImageNamed:@"chevron.left" withConfiguration:arrow_config];
  UIImage* right_image =
      [UIImage systemImageNamed:@"chevron.right" withConfiguration:arrow_config];
  dpad_arrow_up_ = [[UIImageView alloc] initWithImage:up_image];
  dpad_arrow_down_ = [[UIImageView alloc] initWithImage:down_image];
  dpad_arrow_left_ = [[UIImageView alloc] initWithImage:left_image];
  dpad_arrow_right_ = [[UIImageView alloc] initWithImage:right_image];
  for (UIImageView* arrow in
       @[ dpad_arrow_up_, dpad_arrow_down_, dpad_arrow_left_, dpad_arrow_right_ ]) {
    arrow.contentMode = UIViewContentModeCenter;
    arrow.userInteractionEnabled = NO;
    arrow.alpha = 0.85f;
    arrow.tintColor = [[UIColor whiteColor] colorWithAlphaComponent:0.92];
    arrow.hidden = YES;
    [self addSubview:arrow];
  }
}

- (BOOL)isMoveDpadComboControl {
  return control_.type == xe::hid::touch::IOSTouchControlType::kMoveStick &&
         control_.move_with_dpad_ring;
}

- (void)applyControlDefinition:
    (const xe::hid::touch::IOSTouchControlDefinition&)control {
  control_ = control;

  NSString* label_text = xe::ui::XeniaTouchVisibleControlLabelText(control_, NO);
  label_.text = label_text;
  label_.hidden = chrome_suppressed_ || label_text.length == 0;
  behavior_badge_.text = XeniaTouchBehaviorBadgeText(control_);
  [self updateBehaviorBadgeVisibility];

  if ([self isMoveDpadComboControl]) {
    [self ensureDpadArrowViews];
    UIColor* tint =
        xe::ui::XeniaTouchOverlayAccentColor(control_.tint_style, control_.type);
    for (UIImageView* arrow in
         @[ dpad_arrow_up_, dpad_arrow_down_, dpad_arrow_left_, dpad_arrow_right_ ]) {
      arrow.tintColor = tint;
      arrow.hidden = NO;
    }
  } else {
    dpad_arrow_up_.hidden = YES;
    dpad_arrow_down_.hidden = YES;
    dpad_arrow_left_.hidden = YES;
    dpad_arrow_right_.hidden = YES;
  }

  touch_active_ = NO;
  [self refreshVisualState];
  [self setNeedsLayout];
}

- (void)updateBehaviorBadgeVisibility {
  behavior_badge_.hidden = chrome_suppressed_ || !behavior_annotations_visible_ ||
                           behavior_badge_.text.length == 0;
}

- (void)setBehaviorAnnotationsVisible:(BOOL)visible {
  if (behavior_annotations_visible_ == visible) {
    return;
  }
  behavior_annotations_visible_ = visible;
  [self updateBehaviorBadgeVisibility];
}

- (CGFloat)baseVisualAlpha {
  return static_cast<CGFloat>(
      control_.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone
          ? 1.0f
          : std::clamp(control_.visual_opacity, 0.0f, 1.0f));
}

- (void)refreshVisualState {
  if (chrome_suppressed_) {
    [UIView animateWithDuration:0.10
        delay:0.0
        options:(UIViewAnimationOptions)(UIViewAnimationOptionCurveEaseOut |
                                         UIViewAnimationOptionAllowUserInteraction |
                                         UIViewAnimationOptionBeginFromCurrentState)
        animations:^{
          self.alpha = 0.0;
          self.backgroundColor = [UIColor clearColor];
          self.transform = CGAffineTransformIdentity;
        }
        completion:nil];
    [CATransaction begin];
    [CATransaction setAnimationDuration:0.10];
    [CATransaction setAnimationTimingFunction:
                       [CAMediaTimingFunction functionWithName:
                                                 kCAMediaTimingFunctionEaseOut]];
    self.layer.borderColor = [UIColor clearColor].CGColor;
    self.layer.borderWidth = 0.0;
    [CATransaction commit];
    return;
  }

  const CGFloat base_alpha = [self baseVisualAlpha];
  const CGFloat active_alpha = MIN(base_alpha + 0.18f, 1.0f);
  const CGFloat target_alpha = touch_active_ ? active_alpha : base_alpha;
  CGFloat fill_alpha = touch_active_ ? MIN(base_alpha + 0.45f, 1.0f) : base_alpha;
  if (control_.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
    fill_alpha = touch_active_ ? 0.55f : 0.18f;
  }
  UIColor* target_fill =
      xe::ui::XeniaTouchOverlayFillColorForControl(control_, fill_alpha);
  UIColor* target_border =
      xe::ui::XeniaTouchOverlayBorderColorForControl(control_);
  if (conflict_highlighted_) {
    target_border = [[XeniaTheme statusError] colorWithAlphaComponent:0.95];
  }
  if (touch_active_) {
    target_border =
        control_.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone
            ? xe::ui::XeniaTouchOverlayAccentColor(control_.tint_style,
                                                   control_.type)
            : [[UIColor whiteColor] colorWithAlphaComponent:0.78];
  }
  const CGFloat target_border_width =
      control_.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone
          ? (touch_active_ ? 1.8f : 1.0f)
          : (touch_active_ ? 2.0f : 1.5f);
  const CGAffineTransform target_transform =
      touch_active_ ? CGAffineTransformMakeScale(1.03f, 1.03f)
                    : CGAffineTransformIdentity;

  [UIView animateWithDuration:0.08
      delay:0.0
      options:(UIViewAnimationOptions)(UIViewAnimationOptionCurveEaseOut |
                                       UIViewAnimationOptionAllowUserInteraction |
                                       UIViewAnimationOptionBeginFromCurrentState)
      animations:^{
        self.alpha = target_alpha;
        self.backgroundColor = target_fill;
        self.transform = target_transform;
      }
      completion:nil];

  [CATransaction begin];
  [CATransaction setAnimationDuration:0.08];
  [CATransaction setAnimationTimingFunction:
                     [CAMediaTimingFunction functionWithName:
                                               kCAMediaTimingFunctionEaseOut]];
  self.layer.borderColor = target_border.CGColor;
  self.layer.borderWidth = target_border_width;
  [CATransaction commit];
}

- (void)setTouchActive:(BOOL)active {
  if (touch_active_ == active) {
    return;
  }
  touch_active_ = active;
  [self refreshVisualState];
}

- (void)setConflictHighlighted:(BOOL)highlighted {
  if (conflict_highlighted_ == highlighted) {
    return;
  }
  conflict_highlighted_ = highlighted;
  [self refreshVisualState];
}

- (void)setChromeSuppressed:(BOOL)suppressed {
  if (chrome_suppressed_ == suppressed) {
    return;
  }
  chrome_suppressed_ = suppressed;
  label_.hidden = suppressed || (label_.text.length == 0);
  [self updateBehaviorBadgeVisibility];
  [self refreshVisualState];
}

- (void)layoutSubviews {
  [super layoutSubviews];

  CGFloat corner_radius = 16.0f;
  if (control_.shape == xe::hid::touch::IOSTouchControlShape::kCircle) {
    corner_radius =
        MIN(CGRectGetWidth(self.bounds), CGRectGetHeight(self.bounds)) * 0.5f;
  } else if (control_.type ==
             xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
    corner_radius = 22.0f;
  }
  self.layer.cornerRadius = corner_radius;

  label_.frame = CGRectInset(self.bounds, 8.0f, 8.0f);
  if (control_.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
    xe_apply_label_font(label_, UIFontTextStyleCaption2, 11.0,
                        UIFontWeightMedium);
    label_.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.30];
  } else {
    xe_apply_label_font(label_, UIFontTextStyleCaption1, 12.0,
                        UIFontWeightSemibold);
    label_.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.92];
  }

  if ([self isMoveDpadComboControl] && dpad_arrow_up_) {
    const CGFloat short_side =
        MIN(CGRectGetWidth(self.bounds), CGRectGetHeight(self.bounds));
    const CGFloat stick_radius =
        short_side * xe::ui::kXeniaTouchComboStickRadiusFraction;
    const CGFloat outer_radius = short_side * 0.5f;
    const CGFloat arrow_radius = (stick_radius + outer_radius) * 0.5f;
    const CGFloat arrow_size = MAX(short_side * 0.18f, 24.0f);
    const CGFloat centre_x = CGRectGetMidX(self.bounds);
    const CGFloat centre_y = CGRectGetMidY(self.bounds);
    dpad_arrow_up_.frame =
        CGRectMake(centre_x - arrow_size * 0.5f,
                   centre_y - arrow_radius - arrow_size * 0.5f, arrow_size,
                   arrow_size);
    dpad_arrow_down_.frame =
        CGRectMake(centre_x - arrow_size * 0.5f,
                   centre_y + arrow_radius - arrow_size * 0.5f, arrow_size,
                   arrow_size);
    dpad_arrow_left_.frame =
        CGRectMake(centre_x - arrow_radius - arrow_size * 0.5f,
                   centre_y - arrow_size * 0.5f, arrow_size, arrow_size);
    dpad_arrow_right_.frame =
        CGRectMake(centre_x + arrow_radius - arrow_size * 0.5f,
                   centre_y - arrow_size * 0.5f, arrow_size, arrow_size);
  }

  const CGFloat badge_max_width =
      MIN(MAX(CGRectGetWidth(self.bounds) * 0.46f, 18.0f), 38.0f);
  CGSize badge_size =
      [behavior_badge_ sizeThatFits:CGSizeMake(badge_max_width, 14.0f)];
  badge_size.width = MIN(std::ceil(badge_size.width) + 6.0f, badge_max_width);
  badge_size.height = 12.0f;
  const CGFloat badge_x =
      MAX(2.0f, CGRectGetWidth(self.bounds) - badge_size.width - 3.0f);
  const CGFloat badge_y = 3.0f;
  behavior_badge_.frame =
      CGRectIntegral(CGRectMake(badge_x, badge_y, badge_size.width, badge_size.height));
}

@end
