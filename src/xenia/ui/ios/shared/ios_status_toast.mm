/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_status_toast.h"

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {

UIColor* AccentForStyle(XeniaIOSStatusToastStyle style) {
  switch (style) {
    case XeniaIOSStatusToastStyleSuccess:
      return [XeniaTheme accent];
    case XeniaIOSStatusToastStyleWarning:
      return [XeniaTheme statusWarning];
    case XeniaIOSStatusToastStyleError:
      return [XeniaTheme statusError];
    case XeniaIOSStatusToastStyleInfo:
    default:
      return [XeniaTheme textSecondary];
  }
}

NSString* SymbolForStyle(XeniaIOSStatusToastStyle style) {
  switch (style) {
    case XeniaIOSStatusToastStyleSuccess:
      return @"checkmark.circle.fill";
    case XeniaIOSStatusToastStyleWarning:
      return @"exclamationmark.triangle.fill";
    case XeniaIOSStatusToastStyleError:
      return @"xmark.octagon.fill";
    case XeniaIOSStatusToastStyleInfo:
    default:
      return @"info.circle.fill";
  }
}

}  // namespace

@implementation XeniaIOSStatusToastPresenter {
  UIView* toast_;
  UILabel* label_;
  NSTimer* timer_;
}

- (void)dealloc {
  [self dismiss];
  [super dealloc];
}

- (void)dismiss {
  [timer_ invalidate];
  [timer_ release];
  timer_ = nil;
  [toast_.layer removeAllAnimations];
  [toast_ removeFromSuperview];
  [toast_ release];
  toast_ = nil;
  label_ = nil;
}

- (void)timerFired:(NSTimer*)timer {
  if (timer != timer_) {
    return;
  }
  [timer_ release];
  timer_ = nil;
  UIView* toast = [toast_ retain];
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0.12 : 0.24
      delay:0
      options:UIViewAnimationOptionCurveEaseIn | UIViewAnimationOptionAllowUserInteraction
      animations:^{
        toast.alpha = 0.0;
        toast.transform = UIAccessibilityIsReduceMotionEnabled()
                              ? CGAffineTransformIdentity
                              : CGAffineTransformMakeTranslation(0.0, -10.0);
      }
      completion:^(__unused BOOL finished) {
        if (toast == self->toast_) {
          [self dismiss];
        }
        [toast release];
      }];
}

- (void)presentMessage:(NSString*)message
                 style:(XeniaIOSStatusToastStyle)style
                inView:(UIView*)view {
  [self presentMessage:message style:style inView:view duration:3.4];
}

- (void)presentMessage:(NSString*)message
                 style:(XeniaIOSStatusToastStyle)style
                inView:(UIView*)view
              duration:(NSTimeInterval)duration {
  if (!view || message.length == 0) {
    return;
  }
  [self dismiss];

  UIView* toast = [[[UIView alloc] init] autorelease];
  toast.translatesAutoresizingMaskIntoConstraints = NO;
  toast.userInteractionEnabled = NO;
  // Glass on iOS 26+, SystemMaterial on iOS 18-25. The card chrome sets
  // backgroundColor=clear, cornerRadius=XeniaRadiusLg, the hairline border,
  // and inserts the visual-effect backdrop at index 0 — content subviews
  // (icon + label row) stack above it.
  xe_apply_glass_card_chrome(toast, NO);
  xe_apply_shadow_token(toast, XeniaShadowElevationMedium);
  toast.alpha = 0.0;
  toast.transform = UIAccessibilityIsReduceMotionEnabled()
                        ? CGAffineTransformIdentity
                        : CGAffineTransformMakeTranslation(0, -12.0);

  UIImageView* icon = [[[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:SymbolForStyle(style)]] autorelease];
  icon.translatesAutoresizingMaskIntoConstraints = NO;
  icon.tintColor = AccentForStyle(style);

  UILabel* label = [[[UILabel alloc] init] autorelease];
  label.translatesAutoresizingMaskIntoConstraints = NO;
  label.text = message;
  label.textColor = [XeniaTheme textPrimary];
  label.numberOfLines = 2;
  xe_apply_label_font(label, UIFontTextStyleFootnote, 13.0, UIFontWeightSemibold);
  label_ = label;

  UIStackView* row = [[[UIStackView alloc] initWithArrangedSubviews:@[ icon, label ]] autorelease];
  row.translatesAutoresizingMaskIntoConstraints = NO;
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  row.spacing = 10.0;
  row.layoutMargins = UIEdgeInsetsMake(10, 12, 10, 14);
  row.layoutMarginsRelativeArrangement = YES;
  [toast addSubview:row];
  [view addSubview:toast];

  UILayoutGuide* safe = view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [toast.topAnchor constraintEqualToAnchor:safe.topAnchor constant:14],
    [toast.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
    [toast.widthAnchor constraintLessThanOrEqualToAnchor:safe.widthAnchor constant:-32],
    [toast.widthAnchor constraintGreaterThanOrEqualToConstant:220],
    [row.topAnchor constraintEqualToAnchor:toast.topAnchor],
    [row.leadingAnchor constraintEqualToAnchor:toast.leadingAnchor],
    [row.trailingAnchor constraintEqualToAnchor:toast.trailingAnchor],
    [row.bottomAnchor constraintEqualToAnchor:toast.bottomAnchor],
    [icon.widthAnchor constraintEqualToConstant:20],
    [icon.heightAnchor constraintEqualToConstant:20],
  ]];

  XEApplyAccessibility(toast, message, nil, nil, UIAccessibilityTraitStaticText);
  UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, message);

  toast_ = [toast retain];
  UINotificationFeedbackGenerator* feedback =
      [[[UINotificationFeedbackGenerator alloc] init] autorelease];
  [feedback notificationOccurred:style == XeniaIOSStatusToastStyleError
                                     ? UINotificationFeedbackTypeError
                                     : UINotificationFeedbackTypeWarning];
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0.12 : 0.22
                   animations:^{
                     toast.alpha = 1.0;
                     toast.transform = CGAffineTransformIdentity;
                   }];
  if (duration > 0.0) {
    timer_ = [[NSTimer scheduledTimerWithTimeInterval:duration
                                               target:self
                                             selector:@selector(timerFired:)
                                             userInfo:nil
                                              repeats:NO] retain];
  }
}

- (void)updateMessage:(NSString*)message {
  if (!toast_ || !label_ || message.length == 0) {
    return;
  }
  label_.text = message;
  XEApplyAccessibility(toast_, message, nil, nil, UIAccessibilityTraitStaticText);
}

@end
