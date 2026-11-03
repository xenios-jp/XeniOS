/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/game/ios_shader_compile_indicator.h"

#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "xenia/gpu/metal/metal_compile_activity.h"

namespace {

NSString* const kShaderCompileStatusKey = @"XeniaShaderCompileStatusEnabled";
NSString* const kIndeterminateAnimationKey = @"indeterminate";

// A backlog this deep shows at once; a smaller one only after it has lasted
// kSustainSeconds, so the odd shader compiled during play never flashes the capsule.
constexpr uint64_t kBurstBacklog = 8;
constexpr CFTimeInterval kSustainSeconds = 0.5;
// Once the backlog is gone the capsule stays this long, and never less than
// kMinVisibleSeconds in all, so a backlog that drains and refills doesn't flicker.
constexpr CFTimeInterval kIdleHideSeconds = 0.8;
constexpr CFTimeInterval kMinVisibleSeconds = 1.5;
// How long the first-run explanation stays open before folding into the capsule.
constexpr CFTimeInterval kExpandedSeconds = 6.0;

constexpr CGFloat kTopMargin = 8.0;
constexpr CGFloat kSideMargin = 16.0;
constexpr CGFloat kHorizontalInset = 16.0;
constexpr CGFloat kVerticalInset = 9.0;
constexpr CGFloat kBarHeight = 3.0;
constexpr CGFloat kIconSpacing = 7.0;
constexpr CGFloat kRowSpacing = 4.0;
constexpr CGFloat kBarSpacing = 6.0;
constexpr CGFloat kExpandedWidth = 320.0;
constexpr CGFloat kMaxCornerRadius = 24.0;

enum class Phase { kNone, kLoadingCache, kCompiling };

UIFont* CappedFont(UIFontTextStyle style, CGFloat size, UIFontWeight weight, CGFloat maximum) {
  UIFont* font = [UIFont monospacedDigitSystemFontOfSize:size weight:weight];
  return [[UIFontMetrics metricsForTextStyle:style] scaledFontForFont:font
                                                     maximumPointSize:maximum];
}

UIVisualEffect* BackdropEffect() {
  if (@available(iOS 26.0, *)) {
    return [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
  }
  return [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemThinMaterial];
}

}  // namespace

BOOL xe_ios_shader_compile_status_enabled(void) {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if ([defaults objectForKey:kShaderCompileStatusKey] == nil) {
    return YES;
  }
  return [defaults boolForKey:kShaderCompileStatusKey];
}

void xe_ios_set_shader_compile_status_enabled(BOOL enabled) {
  [[NSUserDefaults standardUserDefaults] setBool:enabled forKey:kShaderCompileStatusKey];
}

// The thin bar: filled to progress, or with a short segment sweeping it (under Reduce
// Motion, the whole bar breathing) while the total isn't known.
@interface XeniaCompileProgressBar : UIView
// < 0: indeterminate.
@property(nonatomic, assign) CGFloat progress;
- (void)stopAnimating;
@end

@implementation XeniaCompileProgressBar {
  UIView* fill_;
}

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    _progress = -1.0;
    self.backgroundColor = [UIColor tertiarySystemFillColor];
    self.layer.cornerRadius = kBarHeight / 2.0;
    self.clipsToBounds = YES;
    fill_ = [[UIView alloc] init];
    fill_.backgroundColor = [UIColor labelColor];
    fill_.layer.cornerRadius = kBarHeight / 2.0;
    [self addSubview:fill_];
  }
  return self;
}

- (void)dealloc {
  [fill_ release];
  [super dealloc];
}

- (void)setProgress:(CGFloat)progress {
  if (_progress == progress) {
    return;
  }
  _progress = progress;
  [self setNeedsLayout];
}

- (void)stopAnimating {
  [fill_.layer removeAnimationForKey:kIndeterminateAnimationKey];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  CGFloat width = CGRectGetWidth(self.bounds);
  CGFloat height = CGRectGetHeight(self.bounds);
  if (_progress >= 0.0) {
    [self stopAnimating];
    fill_.frame = CGRectMake(0.0, 0.0, width * std::min<CGFloat>(_progress, 1.0), height);
    return;
  }
  BOOL reduce_motion = UIAccessibilityIsReduceMotionEnabled();
  CGFloat segment = reduce_motion ? width : std::round(width * 0.3);
  fill_.frame = CGRectMake(0.0, 0.0, segment, height);
  // Restart on a size change, so the sweep spans the current width.
  [self stopAnimating];
  if (width <= 0.0 || self.hidden) {
    return;
  }
  CABasicAnimation* animation;
  if (reduce_motion) {
    animation = [CABasicAnimation animationWithKeyPath:@"opacity"];
    animation.fromValue = @(0.25);
    animation.toValue = @(0.6);
    animation.autoreverses = YES;
    animation.duration = 1.2;
  } else {
    animation = [CABasicAnimation animationWithKeyPath:@"position.x"];
    animation.fromValue = @(-segment / 2.0);
    animation.toValue = @(width + segment / 2.0);
    animation.duration = 1.4;
  }
  animation.repeatCount = HUGE_VALF;
  animation.timingFunction =
      [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
  [fill_.layer addAnimation:animation forKey:kIndeterminateAnimationKey];
}

@end

// The capsule's chrome: material backdrop with a capsule (or, when tall, rounded)
// outline and a soft shadow that follows it.
@interface XeniaCompileCapsuleView : UIView
@property(nonatomic, readonly) UIVisualEffectView* backdrop;
@end

@implementation XeniaCompileCapsuleView

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    self.layer.shadowColor = [UIColor blackColor].CGColor;
    self.layer.shadowOpacity = 0.16f;
    self.layer.shadowRadius = 12.0;
    self.layer.shadowOffset = CGSizeMake(0.0, 4.0);
    _backdrop = [[UIVisualEffectView alloc] initWithEffect:BackdropEffect()];
    _backdrop.frame = self.bounds;
    _backdrop.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _backdrop.layer.cornerCurve = kCACornerCurveContinuous;
    _backdrop.clipsToBounds = YES;
    [self addSubview:_backdrop];
  }
  return self;
}

- (void)dealloc {
  [_backdrop release];
  [super dealloc];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  CGFloat radius = std::min(CGRectGetHeight(self.bounds) / 2.0, kMaxCornerRadius);
  _backdrop.layer.cornerRadius = radius;
  self.layer.shadowPath =
      [UIBezierPath bezierPathWithRoundedRect:self.bounds cornerRadius:radius].CGPath;
}

@end

// The indicator itself is a full-size, touch-transparent layer over the game. It lays the
// capsule out by hand: with no constraints, a count changing ten times a second lays out
// only these views, never the host's (whose layout pass reaches the presenter).
@implementation XeniaShaderCompileIndicator {
  CADisplayLink* display_link_;
  XeniaCompileCapsuleView* capsule_;
  UIImageView* icon_;
  UILabel* title_label_;
  UILabel* detail_label_;
  XeniaCompileProgressBar* bar_;

  BOOL shown_;
  BOOL expanded_;
  Phase phase_;
  // < 0 while the total isn't known: the bar runs indeterminate.
  CGFloat progress_;
  uint64_t backlog_;

  CFTimeInterval backlog_since_;
  CFTimeInterval cache_since_;
  CFTimeInterval idle_since_;
  CFTimeInterval shown_at_;
  CFTimeInterval expanded_until_;
  uint32_t storage_generation_;
  // The first-run explanation already opened for this storage generation.
  BOOL explained_generation_;
}

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (!self) {
    return nil;
  }
  self.userInteractionEnabled = NO;
  self.backgroundColor = [UIColor clearColor];
  self.hidden = YES;
  progress_ = -1.0;

  capsule_ = [[XeniaCompileCapsuleView alloc] init];
  capsule_.alpha = 0.0;
  capsule_.isAccessibilityElement = YES;
  capsule_.accessibilityTraits =
      UIAccessibilityTraitStaticText | UIAccessibilityTraitUpdatesFrequently;
  [self addSubview:capsule_];

  icon_ = [[UIImageView alloc] init];
  icon_.tintColor = [UIColor secondaryLabelColor];
  icon_.contentMode = UIViewContentModeCenter;
  icon_.preferredSymbolConfiguration =
      [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleFootnote
                                                       scale:UIImageSymbolScaleMedium];
  [capsule_ addSubview:icon_];

  title_label_ = [[UILabel alloc] init];
  title_label_.font = CappedFont(UIFontTextStyleFootnote, 13.0, UIFontWeightSemibold, 17.0);
  title_label_.adjustsFontForContentSizeCategory = YES;
  title_label_.textColor = [UIColor labelColor];
  [capsule_ addSubview:title_label_];

  detail_label_ = [[UILabel alloc] init];
  detail_label_.font = CappedFont(UIFontTextStyleFootnote, 13.0, UIFontWeightRegular, 17.0);
  detail_label_.adjustsFontForContentSizeCategory = YES;
  detail_label_.textColor = [UIColor secondaryLabelColor];
  detail_label_.numberOfLines = 0;
  detail_label_.text = @"Expect stutter and missing effects for a few minutes.";
  detail_label_.alpha = 0.0;
  [capsule_ addSubview:detail_label_];

  bar_ = [[XeniaCompileProgressBar alloc] init];
  [capsule_ addSubview:bar_];

  [self registerForTraitChanges:@[ UITraitPreferredContentSizeCategory.class ]
                     withAction:@selector(setNeedsLayout)];
  [self applyContentAnimated:NO];
  return self;
}

- (void)dealloc {
  [display_link_ invalidate];
  [display_link_ release];
  [capsule_ release];
  [icon_ release];
  [title_label_ release];
  [detail_label_ release];
  [bar_ release];
  [super dealloc];
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  return nil;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  // Top center inside the safe area: below the status bar and Dynamic Island, clear of
  // the touch controls. Compact, the capsule hugs its one line; open, it takes a fixed
  // width and wraps the explanation.
  UIEdgeInsets safe = self.safeAreaInsets;
  CGFloat safe_width = CGRectGetWidth(self.bounds) - safe.left - safe.right;
  CGFloat max_content = std::max<CGFloat>(
      0.0, (expanded_ ? std::min(kExpandedWidth, safe_width - 2.0 * kSideMargin)
                      : safe_width - 2.0 * kSideMargin) -
               2.0 * kHorizontalInset);
  CGSize icon = icon_.image ? icon_.image.size : CGSizeZero;
  CGFloat title_max = std::max<CGFloat>(0.0, max_content - icon.width - kIconSpacing);
  CGSize title = [title_label_ sizeThatFits:CGSizeMake(title_max, CGFLOAT_MAX)];
  title.width = std::min(std::ceil(title.width), title_max);
  title.height = std::ceil(title.height);
  CGFloat row_height = std::max(icon.height, title.height);
  CGFloat content_width =
      expanded_ ? max_content : std::ceil(icon.width + kIconSpacing + title.width);
  CGFloat detail_height =
      expanded_ ? std::ceil([detail_label_ sizeThatFits:CGSizeMake(content_width, CGFLOAT_MAX)]
                                .height)
                : 0.0;

  CGFloat y = kVerticalInset;
  icon_.frame = CGRectMake(kHorizontalInset, y + (row_height - icon.height) / 2.0, icon.width,
                           icon.height);
  title_label_.frame = CGRectMake(kHorizontalInset + icon.width + kIconSpacing,
                                  y + (row_height - title.height) / 2.0, title.width, title.height);
  y += row_height + kRowSpacing;
  detail_label_.frame = CGRectMake(kHorizontalInset, y, content_width, detail_height);
  if (expanded_) {
    y += detail_height + kBarSpacing;
  }
  bar_.frame = CGRectMake(kHorizontalInset, y, content_width, kBarHeight);
  y += kBarHeight + kVerticalInset;

  // bounds and center, not frame: the capsule may carry its show/hide transform.
  CGSize size = CGSizeMake(content_width + 2.0 * kHorizontalInset, y);
  capsule_.bounds = CGRectMake(0.0, 0.0, size.width, size.height);
  capsule_.center =
      CGPointMake(safe.left + safe_width / 2.0, safe.top + kTopMargin + size.height / 2.0);
}

- (void)setActive:(BOOL)active {
  if (_active == active) {
    return;
  }
  _active = active;
  if (active) {
    display_link_ = [[CADisplayLink displayLinkWithTarget:self selector:@selector(poll:)] retain];
    display_link_.preferredFrameRateRange = CAFrameRateRangeMake(10.0f, 10.0f, 10.0f);
    [display_link_ addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    return;
  }
  [display_link_ invalidate];
  [display_link_ release];
  display_link_ = nil;
  backlog_since_ = 0.0;
  cache_since_ = 0.0;
  idle_since_ = 0.0;
  [self hideAnimated:NO];
}

- (void)poll:(CADisplayLink*)link {
  const xe::gpu::metal::CompileActivity& activity = xe::gpu::metal::compile_activity;
  CFTimeInterval now = link.timestamp;
  uint64_t queued = activity.queued.load(std::memory_order_relaxed);
  uint64_t finished = activity.finished.load(std::memory_order_relaxed);
  uint64_t backlog = queued > finished ? queued - finished : 0;

  uint32_t generation = activity.storage_generation.load(std::memory_order_relaxed);
  if (generation != storage_generation_) {
    storage_generation_ = generation;
    explained_generation_ = NO;
  }
  bool storage_empty = activity.storage_empty.load(std::memory_order_relaxed);
  uint64_t storage_queued_end = activity.storage_queued_end.load(std::memory_order_relaxed);
  bool loading_cache = activity.storage_loading.load(std::memory_order_relaxed) &&
                       !storage_empty &&
                       (storage_queued_end == 0 || finished < storage_queued_end);

  Phase phase = Phase::kNone;
  CGFloat progress = -1.0;
  if (loading_cache) {
    phase = Phase::kLoadingCache;
    uint64_t queued_begin = activity.storage_queued_begin.load(std::memory_order_relaxed);
    uint64_t finished_begin = activity.storage_finished_begin.load(std::memory_order_relaxed);
    if (storage_queued_end > queued_begin) {
      double done = double(finished > finished_begin ? finished - finished_begin : 0);
      progress = CGFloat(std::min(1.0, done / double(storage_queued_end - queued_begin)));
    }
  } else if (backlog) {
    phase = Phase::kCompiling;
  }

  // Timers for showing: a phase has to last, or the backlog be deep.
  if (phase == Phase::kLoadingCache) {
    cache_since_ = cache_since_ ? cache_since_ : now;
  } else {
    cache_since_ = 0.0;
  }
  if (backlog) {
    backlog_since_ = backlog_since_ ? backlog_since_ : now;
  } else {
    backlog_since_ = 0.0;
  }

  if (!xe_ios_shader_compile_status_enabled()) {
    if (shown_) {
      [self hideAnimated:YES];
    }
    return;
  }

  if (!shown_) {
    BOOL wants = (phase == Phase::kLoadingCache && now - cache_since_ >= kSustainSeconds) ||
                 (phase == Phase::kCompiling &&
                  (backlog >= kBurstBacklog || now - backlog_since_ >= kSustainSeconds));
    if (!wants) {
      return;
    }
    phase_ = phase;
    progress_ = progress;
    backlog_ = backlog;
    // The title had nothing stored: say once what that means for the next minutes.
    if (storage_empty && !explained_generation_) {
      explained_generation_ = YES;
      expanded_ = YES;
      expanded_until_ = now + kExpandedSeconds;
    } else {
      expanded_ = NO;
    }
    shown_at_ = now;
    idle_since_ = 0.0;
    [self showAnimated];
    return;
  }

  if (phase == Phase::kNone) {
    // Drained: hold the last state briefly, a determinate bar full, then go.
    idle_since_ = idle_since_ ? idle_since_ : now;
    if (now - idle_since_ >= kIdleHideSeconds && now - shown_at_ >= kMinVisibleSeconds) {
      [self hideAnimated:YES];
      return;
    }
    if (progress_ >= 0.0) {
      progress_ = 1.0;
      bar_.progress = 1.0;
    }
    return;
  }
  idle_since_ = 0.0;

  BOOL collapse = expanded_ && now >= expanded_until_;
  BOOL layout_changed = collapse || phase != phase_;
  if (collapse) {
    expanded_ = NO;
  }
  phase_ = phase;
  backlog_ = backlog;
  progress_ = progress;
  [self applyContentAnimated:layout_changed];
}

- (NSString*)titleText {
  if (expanded_) {
    return @"Building shaders for this game";
  }
  if (phase_ == Phase::kLoadingCache) {
    return @"Loading shader cache";
  }
  return [NSString stringWithFormat:@"Compiling shaders · %llu", (unsigned long long)backlog_];
}

- (NSString*)spokenText {
  if (expanded_) {
    return @"Building shaders for this game. Expect stutter and missing effects for a few "
           @"minutes.";
  }
  if (phase_ == Phase::kLoadingCache) {
    return @"Loading shader cache. Expect stutter until it finishes.";
  }
  return @"Compiling shaders. Expect stutter and missing effects until they finish.";
}

- (void)applyContentAnimated:(BOOL)animated {
  icon_.image = [UIImage systemImageNamed:phase_ == Phase::kLoadingCache
                                              ? @"square.stack.3d.down.right"
                                              : @"cube.transparent"];
  NSString* title = [self titleText];
  if (![title_label_.text isEqualToString:title]) {
    title_label_.text = title;
  }
  title_label_.numberOfLines = expanded_ ? 0 : 1;
  bar_.progress = progress_;
  [self setNeedsLayout];
  capsule_.accessibilityLabel = [self spokenText];
  capsule_.accessibilityValue =
      progress_ >= 0.0
          ? [NSNumberFormatter localizedStringFromNumber:@(progress_)
                                             numberStyle:NSNumberFormatterPercentStyle]
          : nil;

  void (^layout)(void) = ^{
    detail_label_.alpha = expanded_ ? 1.0 : 0.0;
    [self layoutIfNeeded];
  };
  if (!animated || UIAccessibilityIsReduceMotionEnabled()) {
    detail_label_.alpha = expanded_ ? 1.0 : 0.0;
    return;
  }
  [UIView animateWithDuration:0.5
                        delay:0.0
       usingSpringWithDamping:0.86
        initialSpringVelocity:0.0
                      options:UIViewAnimationOptionBeginFromCurrentState
                   animations:layout
                   completion:nil];
}

- (void)showAnimated {
  shown_ = YES;
  self.hidden = NO;
  [self applyContentAnimated:NO];
  // Sized before it springs in, not during.
  [self layoutIfNeeded];
  UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, [self spokenText]);
  if (UIAccessibilityIsReduceMotionEnabled()) {
    capsule_.transform = CGAffineTransformIdentity;
    [UIView animateWithDuration:0.2
                     animations:^{
                       capsule_.alpha = 1.0;
                     }];
    return;
  }
  capsule_.transform = CGAffineTransformConcat(CGAffineTransformMakeScale(0.9, 0.9),
                                               CGAffineTransformMakeTranslation(0.0, -10.0));
  [UIView animateWithDuration:0.5
                        delay:0.0
       usingSpringWithDamping:0.78
        initialSpringVelocity:0.0
                      options:UIViewAnimationOptionBeginFromCurrentState
                   animations:^{
                     capsule_.alpha = 1.0;
                     capsule_.transform = CGAffineTransformIdentity;
                   }
                   completion:nil];
}

- (void)hideAnimated:(BOOL)animated {
  if (!shown_) {
    return;
  }
  shown_ = NO;
  expanded_ = NO;
  idle_since_ = 0.0;
  void (^finish)(BOOL) = ^(BOOL finished) {
    if (!shown_) {
      self.hidden = YES;
      capsule_.transform = CGAffineTransformIdentity;
      [bar_ stopAnimating];
    }
  };
  if (!animated) {
    capsule_.alpha = 0.0;
    finish(YES);
    return;
  }
  BOOL reduce_motion = UIAccessibilityIsReduceMotionEnabled();
  [UIView animateWithDuration:reduce_motion ? 0.2 : 0.35
      delay:0.0
      options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionCurveEaseIn
      animations:^{
        capsule_.alpha = 0.0;
        if (!reduce_motion) {
          capsule_.transform = CGAffineTransformConcat(
              CGAffineTransformMakeScale(0.94, 0.94), CGAffineTransformMakeTranslation(0.0, -6.0));
        }
      }
      completion:finish];
}

@end
