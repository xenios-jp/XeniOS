/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/game/ios_achievement_notification_presenter.h"

#include <algorithm>
#include <deque>
#include <string>

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {

constexpr NSTimeInterval kXeniaAchievementToastHoldSeconds = 4.5;
constexpr CGFloat kXeniaAchievementToastHeight = 74.0;
constexpr CGFloat kXeniaAchievementToastMargin = 18.0;
constexpr size_t kXeniaAchievementToastQueueLimit = 5;

NSString* NSStringFromStdString(const std::string& value) {
  if (value.empty()) {
    return @"";
  }
  NSString* string = [[NSString alloc] initWithBytes:value.data()
                                              length:value.size()
                                            encoding:NSUTF8StringEncoding];
  NSString* result = [string autorelease];
  return result ? result : @"";
}

UIImage* ImageFromPayload(const xe::ui::AchievementNotificationPayload& payload) {
  static NSCache* icon_cache = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    icon_cache = [[NSCache alloc] init];
    icon_cache.countLimit = 32;
  });

  NSString* cache_key = [NSString stringWithFormat:@"%08X:%u", payload.title_id,
                                                   payload.achievement_id];
  UIImage* cached = [icon_cache objectForKey:cache_key];
  if (cached) {
    return cached;
  }

  if (!payload.icon_data.empty()) {
    NSData* data = [NSData dataWithBytes:payload.icon_data.data() length:payload.icon_data.size()];
    UIImage* image = [UIImage imageWithData:data];
    if (image) {
      [icon_cache setObject:image forKey:cache_key];
      return image;
    }
  }
  if (@available(iOS 13.0, *)) {
    return [UIImage systemImageNamed:@"trophy.fill"];
  }
  return nil;
}

xe::ui::AchievementNotificationPayload MakeOverflowPayload(NSUInteger count) {
  xe::ui::AchievementNotificationPayload payload;
  payload.title = "Achievements";
  payload.description = [[NSString stringWithFormat:@"+%lu more unlocked",
                                                    static_cast<unsigned long>(count)] UTF8String];
  return payload;
}

CGFloat FinalToastWidthForView(UIView* host_view) {
  CGFloat safe_width = CGRectGetWidth(host_view.bounds) - host_view.safeAreaInsets.left -
                       host_view.safeAreaInsets.right;
  if (safe_width <= 0.0) {
    safe_width = CGRectGetWidth(host_view.bounds);
  }
  const CGFloat maximum_width = std::max<CGFloat>(0.0, safe_width - 32.0);
  if (maximum_width <= 0.0) {
    return 0.0;
  }
  const CGFloat preferred_width =
      std::max<CGFloat>(300.0, std::min<CGFloat>(520.0, safe_width * 0.52));
  return std::min<CGFloat>(preferred_width, maximum_width);
}

enum class ToastHorizontalPosition {
  kLeft,
  kCenter,
  kRight,
};

enum class ToastVerticalPosition {
  kTop,
  kCenter,
  kBottom,
};

ToastHorizontalPosition HorizontalPosition(uint8_t position_id) {
  switch (position_id) {
    case 4:
    case 5:
    case 6:
      return ToastHorizontalPosition::kLeft;
    case 8:
    case 9:
    case 10:
      return ToastHorizontalPosition::kRight;
    default:
      return ToastHorizontalPosition::kCenter;
  }
}

ToastVerticalPosition VerticalPosition(uint8_t position_id) {
  switch (position_id) {
    case 1:
    case 5:
    case 9:
      return ToastVerticalPosition::kTop;
    case 0:
    case 4:
    case 8:
      return ToastVerticalPosition::kCenter;
    default:
      return ToastVerticalPosition::kBottom;
  }
}

}  // namespace

@interface XeniaIOSAchievementNotificationView : UIView
@property(nonatomic, retain) UIView* textContainerView;
- (instancetype)initWithPayload:(const xe::ui::AchievementNotificationPayload&)payload;
@end

@implementation XeniaIOSAchievementNotificationView

- (instancetype)initWithPayload:(const xe::ui::AchievementNotificationPayload&)payload {
  self = [super initWithFrame:CGRectZero];
  if (!self) {
    return nil;
  }

  self.translatesAutoresizingMaskIntoConstraints = NO;
  self.backgroundColor = [UIColor clearColor];
  self.clipsToBounds = YES;
  self.userInteractionEnabled = YES;
  self.layer.cornerRadius = XeniaRadiusXxl;
  self.layer.borderWidth = 1.0;
  self.layer.borderColor = [XeniaTheme border].CGColor;

  // Glass backdrop on iOS 26+, SystemMaterial fallback on iOS 18-25.
  // Inserted at index 0 so the icon/text row stacks above it. Inlined
  // rather than going through xe_apply_glass_card_chrome because the
  // notification uses XeniaRadiusXxl (24pt) — the helper hardcodes
  // XeniaRadiusLg (12pt) for the smaller toast use case.
  UIVisualEffectView* backdrop =
      [[UIVisualEffectView alloc] initWithEffect:xe_make_chrome_visual_effect(NO)];
  backdrop.frame = self.bounds;
  backdrop.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  backdrop.userInteractionEnabled = NO;
  [self insertSubview:backdrop atIndex:0];
  [backdrop release];

  xe_apply_shadow_token(self, XeniaShadowElevationElevated);

  UIImageView* icon_view =
      [[[UIImageView alloc] initWithImage:ImageFromPayload(payload)] autorelease];
  icon_view.translatesAutoresizingMaskIntoConstraints = NO;
  icon_view.contentMode = UIViewContentModeScaleAspectFit;
  icon_view.tintColor = [XeniaTheme accent];

  UILabel* title_label = [[[UILabel alloc] init] autorelease];
  title_label.text = NSStringFromStdString(payload.title);
  title_label.textColor = [XeniaTheme textPrimary];
  title_label.numberOfLines = 1;
  title_label.adjustsFontForContentSizeCategory = YES;
  title_label.adjustsFontSizeToFitWidth = YES;
  title_label.minimumScaleFactor = 0.82;
  xe_apply_label_font(title_label, UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold);

  UILabel* description_label = [[[UILabel alloc] init] autorelease];
  description_label.text = NSStringFromStdString(payload.description);
  description_label.textColor = [XeniaTheme textSecondary];
  description_label.numberOfLines = 1;
  description_label.adjustsFontForContentSizeCategory = YES;
  description_label.adjustsFontSizeToFitWidth = YES;
  description_label.minimumScaleFactor = 0.78;
  xe_apply_label_font(description_label, UIFontTextStyleFootnote, 13.0, UIFontWeightMedium);

  UIStackView* text_stack = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ title_label, description_label ]] autorelease];
  text_stack.translatesAutoresizingMaskIntoConstraints = NO;
  text_stack.axis = UILayoutConstraintAxisVertical;
  text_stack.spacing = 2.0;
  text_stack.alignment = UIStackViewAlignmentFill;
  self.textContainerView = text_stack;

  UIStackView* row =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ icon_view, text_stack ]] autorelease];
  row.translatesAutoresizingMaskIntoConstraints = NO;
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  row.spacing = 12.0;
  row.layoutMargins = UIEdgeInsetsMake(8.0, 10.0, 8.0, 16.0);
  row.layoutMarginsRelativeArrangement = YES;
  [self addSubview:row];

  [NSLayoutConstraint activateConstraints:@[
    [row.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
    [row.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
    [row.topAnchor constraintEqualToAnchor:self.topAnchor],
    [row.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    [icon_view.widthAnchor constraintEqualToConstant:56.0],
    [icon_view.heightAnchor constraintEqualToConstant:56.0]
  ]];

  self.isAccessibilityElement = YES;
  self.accessibilityLabel =
      [NSString stringWithFormat:@"%@, %@", title_label.text, description_label.text];

  return self;
}

- (void)dealloc {
  [_textContainerView release];
  [super dealloc];
}

@end

@implementation XeniaIOSAchievementNotificationPresenter {
  std::deque<xe::ui::AchievementNotificationPayload> pending_payloads_;
  UIView* host_view_;
  UIView* active_banner_;
  NSTimer* dismissal_timer_;
  NSUInteger overflow_count_;
  BOOL presenting_;
}

- (instancetype)init {
  self = [super init];
  if (!self) {
    return nil;
  }
  host_view_ = nil;
  active_banner_ = nil;
  dismissal_timer_ = nil;
  overflow_count_ = 0;
  presenting_ = NO;
  return self;
}

- (void)dealloc {
  [self dismissAll];
  [super dealloc];
}

- (void)presentPayload:(const xe::ui::AchievementNotificationPayload&)payload inView:(UIView*)view {
  if (!view) {
    return;
  }
  host_view_ = view;
  if (pending_payloads_.size() >= kXeniaAchievementToastQueueLimit) {
    ++overflow_count_;
    return;
  }
  pending_payloads_.push_back(payload);
  if (!presenting_) {
    [self presentNextPayload];
  }
}

- (void)dismissAll {
  [dismissal_timer_ invalidate];
  [dismissal_timer_ release];
  dismissal_timer_ = nil;
  pending_payloads_.clear();
  overflow_count_ = 0;
  if (active_banner_) {
    [active_banner_.layer removeAllAnimations];
    [active_banner_ removeFromSuperview];
    [active_banner_ release];
    active_banner_ = nil;
  }
  presenting_ = NO;
}

- (void)presentNextPayload {
  if (pending_payloads_.empty()) {
    if (overflow_count_ > 0) {
      pending_payloads_.push_back(MakeOverflowPayload(overflow_count_));
      overflow_count_ = 0;
    } else {
      presenting_ = NO;
      return;
    }
  }
  if (pending_payloads_.empty()) {
    presenting_ = NO;
    return;
  }
  if (!host_view_ || !host_view_.window) {
    pending_payloads_.clear();
    presenting_ = NO;
    return;
  }

  presenting_ = YES;
  xe::ui::AchievementNotificationPayload payload = pending_payloads_.front();
  pending_payloads_.pop_front();

  const CGFloat final_width = FinalToastWidthForView(host_view_);
  if (final_width <= 0.0) {
    presenting_ = NO;
    [self presentNextPayload];
    return;
  }

  XeniaIOSAchievementNotificationView* banner =
      [[[XeniaIOSAchievementNotificationView alloc] initWithPayload:payload] autorelease];
  banner.alpha = 0.0;
  banner.transform = CGAffineTransformMakeScale(0.2, 1.0);
  banner.textContainerView.alpha = 0.0;
  active_banner_ = [banner retain];
  UITapGestureRecognizer* tap =
      [[[UITapGestureRecognizer alloc] initWithTarget:self
                                               action:@selector(dismissCurrentBanner:)] autorelease];
  [banner addGestureRecognizer:tap];
  UISwipeGestureRecognizer* swipe_up =
      [[[UISwipeGestureRecognizer alloc] initWithTarget:self
                                                 action:@selector(dismissCurrentBanner:)] autorelease];
  swipe_up.direction = UISwipeGestureRecognizerDirectionUp;
  [banner addGestureRecognizer:swipe_up];
  UISwipeGestureRecognizer* swipe_side =
      [[[UISwipeGestureRecognizer alloc] initWithTarget:self
                                                 action:@selector(dismissCurrentBanner:)] autorelease];
  swipe_side.direction = UISwipeGestureRecognizerDirectionLeft |
                         UISwipeGestureRecognizerDirectionRight;
  [banner addGestureRecognizer:swipe_side];

  [host_view_ addSubview:banner];
  UILayoutGuide* safe_area = host_view_.safeAreaLayoutGuide;
  NSMutableArray<NSLayoutConstraint*>* constraints = [NSMutableArray arrayWithArray:@[
    [banner.widthAnchor constraintEqualToConstant:final_width],
    [banner.heightAnchor constraintEqualToConstant:kXeniaAchievementToastHeight]
  ]];

  switch (HorizontalPosition(payload.position_id)) {
    case ToastHorizontalPosition::kLeft:
      [constraints
          addObject:[banner.leadingAnchor constraintEqualToAnchor:safe_area.leadingAnchor
                                                         constant:kXeniaAchievementToastMargin]];
      break;
    case ToastHorizontalPosition::kRight:
      [constraints
          addObject:[banner.trailingAnchor constraintEqualToAnchor:safe_area.trailingAnchor
                                                          constant:-kXeniaAchievementToastMargin]];
      break;
    case ToastHorizontalPosition::kCenter:
      [constraints
          addObject:[banner.centerXAnchor constraintEqualToAnchor:safe_area.centerXAnchor]];
      break;
  }

  switch (VerticalPosition(payload.position_id)) {
    case ToastVerticalPosition::kTop:
      [constraints
          addObject:[banner.topAnchor constraintEqualToAnchor:safe_area.topAnchor
                                                     constant:kXeniaAchievementToastMargin]];
      break;
    case ToastVerticalPosition::kCenter:
      [constraints
          addObject:[banner.centerYAnchor constraintEqualToAnchor:safe_area.centerYAnchor]];
      break;
    case ToastVerticalPosition::kBottom:
      [constraints
          addObject:[banner.bottomAnchor constraintEqualToAnchor:safe_area.bottomAnchor
                                                        constant:-kXeniaAchievementToastMargin]];
      break;
  }

  [NSLayoutConstraint activateConstraints:constraints];
  [host_view_ layoutIfNeeded];

  UINotificationFeedbackGenerator* feedback =
      [[[UINotificationFeedbackGenerator alloc] init] autorelease];
  [feedback notificationOccurred:UINotificationFeedbackTypeSuccess];
  [self animateBanner:banner];
}

- (void)animateBanner:(XeniaIOSAchievementNotificationView*)banner {
  if (UIAccessibilityIsReduceMotionEnabled()) {
    banner.transform = CGAffineTransformMakeScale(0.98, 0.98);
    banner.textContainerView.alpha = 1.0;
    [UIView animateWithDuration:0.16
        delay:0.0
        options:UIViewAnimationOptionCurveEaseOut | UIViewAnimationOptionAllowUserInteraction
        animations:^{
          banner.alpha = 1.0;
          banner.transform = CGAffineTransformIdentity;
        }
        completion:^(__unused BOOL finished) {
          [self scheduleDismissForBanner:banner reduceMotion:YES];
        }];
    return;
  }

  [UIView animateWithDuration:0.36
      delay:0.0
      options:UIViewAnimationOptionCurveEaseOut | UIViewAnimationOptionAllowUserInteraction
      animations:^{
        banner.alpha = 1.0;
        banner.transform = CGAffineTransformMakeScale(1.1, 1.0);
        banner.textContainerView.alpha = 1.0;
      }
      completion:^(__unused BOOL finished) {
        [UIView animateWithDuration:0.12
            delay:0.0
            options:UIViewAnimationOptionCurveEaseInOut | UIViewAnimationOptionAllowUserInteraction
            animations:^{
              banner.transform = CGAffineTransformIdentity;
            }
            completion:^(__unused BOOL settle_finished) {
              [self scheduleDismissForBanner:banner reduceMotion:NO];
            }];
      }];
}

- (void)scheduleDismissForBanner:(XeniaIOSAchievementNotificationView*)banner
                    reduceMotion:(BOOL)reduce_motion {
  (void)reduce_motion;
  [dismissal_timer_ invalidate];
  [dismissal_timer_ release];
  dismissal_timer_ =
      [[NSTimer scheduledTimerWithTimeInterval:kXeniaAchievementToastHoldSeconds
                                        target:self
                                      selector:@selector(dismissTimerFired:)
                                      userInfo:banner
                                       repeats:NO] retain];
}

- (void)dismissTimerFired:(NSTimer*)timer {
  if (timer != dismissal_timer_) {
    return;
  }
  UIView* banner = (UIView*)timer.userInfo;
  [dismissal_timer_ release];
  dismissal_timer_ = nil;
  if (banner != active_banner_) {
    return;
  }
  [self dismissCurrentBannerAnimated:YES];
}

- (void)dismissCurrentBanner:(id)sender {
  (void)sender;
  [self dismissCurrentBannerAnimated:YES];
}

- (void)dismissCurrentBannerAnimated:(BOOL)animated {
  if (!active_banner_) {
    return;
  }
  [dismissal_timer_ invalidate];
  [dismissal_timer_ release];
  dismissal_timer_ = nil;
  UIView* banner = active_banner_;
  if (!animated || UIAccessibilityIsReduceMotionEnabled()) {
    [UIView animateWithDuration:animated ? 0.18 : 0.0
        delay:0.0
        options:UIViewAnimationOptionCurveEaseIn | UIViewAnimationOptionAllowUserInteraction
        animations:^{
          banner.alpha = 0.0;
        }
        completion:^(__unused BOOL finished) {
          [self finishBanner:banner];
        }];
    return;
  }
  [UIView animateWithDuration:0.32
      delay:0.0
      options:UIViewAnimationOptionCurveEaseIn | UIViewAnimationOptionAllowUserInteraction
      animations:^{
        banner.alpha = 0.0;
        banner.transform = CGAffineTransformMakeScale(0.2, 1.0);
        ((XeniaIOSAchievementNotificationView*)banner).textContainerView.alpha = 0.0;
      }
      completion:^(__unused BOOL finished) {
        [self finishBanner:banner];
      }];
}

- (void)finishBanner:(UIView*)banner {
  if (banner != active_banner_) {
    return;
  }
  [dismissal_timer_ invalidate];
  [dismissal_timer_ release];
  dismissal_timer_ = nil;
  [active_banner_ removeFromSuperview];
  [active_banner_ release];
  active_banner_ = nil;
  presenting_ = NO;
  [self presentNextPayload];
}

@end
