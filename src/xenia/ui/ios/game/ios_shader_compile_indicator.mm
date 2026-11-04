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
#include <deque>
#include <utility>

#include "xenia/gpu/metal/metal_compile_activity.h"
#import "xenia/ui/ios/shared/ios_theme.h"

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
constexpr CGFloat kTopInset = 9.0;
// One more than the top: the progress line runs along the bottom edge.
constexpr CGFloat kBottomInset = 10.0;
constexpr CGFloat kLineHeight = 2.0;
constexpr CGFloat kIconSpacing = 7.0;
constexpr CGFloat kRowSpacing = 4.0;
// The first-run explanation takes one line when it fits, else balanced lines no
// wider than this.
constexpr CGFloat kMaxExpandedContentWidth = 420.0;
constexpr CGFloat kMaxCornerRadius = 24.0;

enum class Phase { kNone, kLoadingCache, kCompiling };

enum class WaitState { kIdle, kOffered, kWaiting };

// The live rate is the completions over this long a window, smoothed over about this
// many seconds.
constexpr CFTimeInterval kRateWindowSeconds = 10.0;
constexpr CFTimeInterval kRateSmoothingSeconds = 3.0;

void (^g_shader_wait_handler)(BOOL) = nil;

NSString* DurationString(NSTimeInterval seconds, NSDateComponentsFormatterUnitsStyle style,
                         NSCalendarUnit unit) {
  NSDateComponentsFormatter* formatter = [[[NSDateComponentsFormatter alloc] init] autorelease];
  formatter.unitsStyle = style;
  formatter.maximumUnitCount = 1;
  formatter.allowedUnits = unit;
  return [formatter stringFromTimeInterval:seconds];
}

// "About 40 seconds left", "About 3 minutes left", or the over-10-minutes line; nil when
// there is nothing to say.
NSString* EtaSentence(const xe::ui::ios::ShaderEta& eta) {
  using Kind = xe::ui::ios::ShaderEta::Kind;
  switch (eta.kind) {
    case Kind::kNone:
      return nil;
    case Kind::kOver:
      return NSLocalizedString(
          @"Over 10 minutes. This is a one-time cost per game and update.",
          @"Shader preparation estimate when it will take more than ten minutes");
    case Kind::kSeconds:
      return [NSString
          stringWithFormat:NSLocalizedString(@"About %@ left",
                                             @"Shader preparation time left; %@ is a duration "
                                             @"such as 40 seconds or 3 minutes"),
                           DurationString(eta.value, NSDateComponentsFormatterUnitsStyleFull,
                                          NSCalendarUnitSecond)];
    case Kind::kMinutes:
      return [NSString
          stringWithFormat:NSLocalizedString(@"About %@ left",
                                             @"Shader preparation time left; %@ is a duration "
                                             @"such as 40 seconds or 3 minutes"),
                           DurationString(eta.value * 60, NSDateComponentsFormatterUnitsStyleFull,
                                          NSCalendarUnitMinute)];
  }
  return nil;
}

// The short form for the capsule's title ("40 sec", "3 min"); nil for none or over 10 minutes.
NSString* EtaShort(const xe::ui::ios::ShaderEta& eta) {
  using Kind = xe::ui::ios::ShaderEta::Kind;
  if (eta.kind == Kind::kSeconds) {
    return DurationString(eta.value, NSDateComponentsFormatterUnitsStyleShort,
                          NSCalendarUnitSecond);
  }
  if (eta.kind == Kind::kMinutes) {
    return DurationString(eta.value * 60, NSDateComponentsFormatterUnitsStyleShort,
                          NSCalendarUnitMinute);
  }
  return nil;
}

UIFont* CappedFont(UIFontTextStyle style, CGFloat size, UIFontWeight weight, CGFloat maximum) {
  UIFont* font = [UIFont monospacedDigitSystemFontOfSize:size weight:weight];
  return [[UIFontMetrics metricsForTextStyle:style] scaledFontForFont:font
                                                     maximumPointSize:maximum];
}

constexpr CGFloat kFontSize = 13.0;
constexpr CGFloat kMaxFontSize = 17.0;

// Always dark, like the system's Game Mode banner, and tinted so the text keeps
// WCAG AA contrast (4.5:1) even over a white frame.
UIVisualEffect* BackdropEffect() {
  UIGlassEffect* glass = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
  glass.tintColor = [UIColor colorWithWhite:0.0 alpha:0.7];
  return glass;
}

// The explanation and the icon: dimmer than the title, brighter than the system's
// secondary label so they keep that contrast too.
UIColor* SecondaryColor() {
  return [UIColor colorWithWhite:1.0 alpha:0.75];
}

}  // namespace

void xe_ios_set_shader_wait_handler(void (^handler)(BOOL waiting)) {
  void (^old)(BOOL) = g_shader_wait_handler;
  g_shader_wait_handler = handler ? [handler copy] : nil;
  [old release];
}

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

// The progress line along the capsule's bottom edge: filled to progress, or with a
// soft highlight sweeping it while the total isn't known (static under Reduce Motion).
@interface XeniaCompileProgressLine : UIView
// < 0: indeterminate.
@property(nonatomic, assign) CGFloat progress;
- (void)stopAnimating;
@end

@implementation XeniaCompileProgressLine {
  UIView* fill_;
  CAGradientLayer* sweep_;
}

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    _progress = -1.0;
    self.userInteractionEnabled = NO;
    self.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.12];
    fill_ = [[UIView alloc] init];
    fill_.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.85];
    [self addSubview:fill_];
    sweep_ = [[CAGradientLayer alloc] init];
    sweep_.startPoint = CGPointMake(0.0, 0.5);
    sweep_.endPoint = CGPointMake(1.0, 0.5);
    sweep_.colors = @[
      (id)[UIColor colorWithWhite:1.0 alpha:0.0].CGColor,
      (id)[UIColor colorWithWhite:1.0 alpha:0.7].CGColor,
      (id)[UIColor colorWithWhite:1.0 alpha:0.0].CGColor,
    ];
    [self.layer addSublayer:sweep_];
  }
  return self;
}

- (void)dealloc {
  [fill_ release];
  [sweep_ release];
  [super dealloc];
}

- (void)setProgress:(CGFloat)progress {
  if (_progress == progress) {
    return;
  }
  BOOL mode_changed = (_progress < 0.0) != (progress < 0.0);
  _progress = progress;
  if (mode_changed) {
    [self stopAnimating];
  }
  [self setNeedsLayout];
}

- (void)stopAnimating {
  [sweep_ removeAnimationForKey:kIndeterminateAnimationKey];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  CGFloat width = CGRectGetWidth(self.bounds);
  CGFloat height = CGRectGetHeight(self.bounds);
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  if (_progress >= 0.0) {
    fill_.hidden = NO;
    sweep_.hidden = YES;
    [self stopAnimating];
    fill_.frame = CGRectMake(0.0, 0.0, width * std::min<CGFloat>(_progress, 1.0), height);
    [CATransaction commit];
    return;
  }
  fill_.hidden = YES;
  BOOL reduce_motion = UIAccessibilityIsReduceMotionEnabled();
  sweep_.hidden = reduce_motion;
  CGFloat segment = std::round(width * 0.35);
  CGRect sweep_bounds = CGRectMake(0.0, 0.0, segment, height);
  BOOL resized = !CGRectEqualToRect(sweep_.bounds, sweep_bounds);
  sweep_.bounds = sweep_bounds;
  sweep_.position = CGPointMake(-segment / 2.0, height / 2.0);
  [CATransaction commit];
  if (reduce_motion || width <= 0.0) {
    [self stopAnimating];
    return;
  }
  // Restart only for a new width, so the sweep spans it; otherwise keep it running.
  if (!resized && [sweep_ animationForKey:kIndeterminateAnimationKey]) {
    return;
  }
  CABasicAnimation* animation = [CABasicAnimation animationWithKeyPath:@"position.x"];
  animation.fromValue = @(-segment / 2.0);
  animation.toValue = @(width + segment / 2.0);
  animation.duration = 1.6;
  animation.repeatCount = HUGE_VALF;
  animation.timingFunction =
      [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
  [sweep_ addAnimation:animation forKey:kIndeterminateAnimationKey];
}

@end

// The "Preparing shaders" sheet: shown over the game when the stored shaders are forecast to take
// a long while. Wait holds the game; Play now (or swiping the sheet away) lets it run while the
// shaders keep building. The indicator feeds it progress and owns the decision.
@interface XeniaShaderPrepareViewController : UIViewController <UIAdaptivePresentationControllerDelegate>
@property(nonatomic, copy) void (^onWait)(void);
@property(nonatomic, copy) void (^onPlayNow)(void);
// |eta| may be nil; |note| is the footnote. progress < 0: unknown.
- (void)setProgress:(CGFloat)progress
                eta:(NSString*)eta
               note:(NSString*)note
            waiting:(BOOL)waiting;
@end

@implementation XeniaShaderPrepareViewController {
  UILabel* title_label_;
  UILabel* eta_label_;
  UILabel* note_label_;
  UIProgressView* progress_view_;
  UIButton* wait_button_;
  UIButton* play_button_;
  CGFloat pending_progress_;
  NSString* pending_eta_;
  NSString* pending_note_;
  BOOL pending_waiting_;
}

- (instancetype)initWithNibName:(NSString*)name bundle:(NSBundle*)bundle {
  self = [super initWithNibName:name bundle:bundle];
  if (self) {
    pending_progress_ = -1.0;
    self.modalPresentationStyle = UIModalPresentationPageSheet;
    UISheetPresentationController* sheet = self.sheetPresentationController;
    sheet.detents = @[ [UISheetPresentationControllerDetent mediumDetent] ];
    sheet.prefersGrabberVisible = YES;
  }
  return self;
}

- (void)dealloc {
  [_onWait release];
  [_onPlayNow release];
  [title_label_ release];
  [eta_label_ release];
  [note_label_ release];
  [progress_view_ release];
  [wait_button_ release];
  [play_button_ release];
  [pending_eta_ release];
  [pending_note_ release];
  [super dealloc];
}

- (UILabel*)labelWithStyle:(UIFontTextStyle)style weight:(UIFontWeight)weight {
  UILabel* label = [[[UILabel alloc] init] autorelease];
  UIFont* base = [UIFont preferredFontForTextStyle:style];
  label.font = [[UIFontMetrics metricsForTextStyle:style]
      scaledFontForFont:[UIFont systemFontOfSize:base.pointSize weight:weight]];
  label.adjustsFontForContentSizeCategory = YES;
  label.numberOfLines = 0;
  label.textAlignment = NSTextAlignmentCenter;
  return label;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor systemBackgroundColor];
  // A swipe-down reports through the presentation controller.
  self.presentationController.delegate = self;

  title_label_ = [[self labelWithStyle:UIFontTextStyleTitle2 weight:UIFontWeightSemibold] retain];
  title_label_.text = NSLocalizedString(@"Preparing shaders", @"Shader preparation sheet title");
  title_label_.accessibilityTraits |= UIAccessibilityTraitHeader;
  eta_label_ = [[self labelWithStyle:UIFontTextStyleBody weight:UIFontWeightRegular] retain];
  note_label_ = [[self labelWithStyle:UIFontTextStyleFootnote weight:UIFontWeightRegular] retain];
  note_label_.textColor = [UIColor secondaryLabelColor];

  progress_view_ = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];

  UIButtonConfiguration* wait_config = [UIButtonConfiguration filledButtonConfiguration];
  wait_config.title = NSLocalizedString(@"Wait", @"Button that waits for shaders to finish building");
  wait_config.baseBackgroundColor = [XeniaTheme textPrimary];
  wait_config.baseForegroundColor = [XeniaTheme bgPrimary];
  wait_config.buttonSize = UIButtonConfigurationSizeLarge;
  wait_config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  wait_button_ = [[UIButton buttonWithConfiguration:wait_config primaryAction:nil] retain];
  [wait_button_ addTarget:self action:@selector(waitTapped) forControlEvents:UIControlEventTouchUpInside];

  UIButtonConfiguration* play_config = [UIButtonConfiguration plainButtonConfiguration];
  play_config.title = NSLocalizedString(@"Play now", @"Button that starts the game while shaders keep building");
  play_config.buttonSize = UIButtonConfigurationSizeLarge;
  play_button_ = [[UIButton buttonWithConfiguration:play_config primaryAction:nil] retain];
  [play_button_ addTarget:self action:@selector(playTapped) forControlEvents:UIControlEventTouchUpInside];

  UIStackView* stack = [[[UIStackView alloc] initWithArrangedSubviews:@[
    title_label_, progress_view_, eta_label_, note_label_, wait_button_, play_button_
  ]] autorelease];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.spacing = 14.0;
  stack.alignment = UIStackViewAlignmentFill;
  [stack setCustomSpacing:22.0 afterView:note_label_];
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:stack];
  UILayoutGuide* guide = self.view.readableContentGuide;
  [NSLayoutConstraint activateConstraints:@[
    [stack.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:28.0],
    [stack.leadingAnchor constraintEqualToAnchor:guide.leadingAnchor],
    [stack.trailingAnchor constraintEqualToAnchor:guide.trailingAnchor],
  ]];
  [self applyState];
}

- (void)setProgress:(CGFloat)progress
                eta:(NSString*)eta
               note:(NSString*)note
            waiting:(BOOL)waiting {
  pending_progress_ = progress;
  if (![pending_eta_ isEqualToString:eta] && pending_eta_ != eta) {
    [pending_eta_ release];
    pending_eta_ = [eta copy];
  }
  if (![pending_note_ isEqualToString:note] && pending_note_ != note) {
    [pending_note_ release];
    pending_note_ = [note copy];
  }
  pending_waiting_ = waiting;
  if (self.isViewLoaded) {
    [self applyState];
  }
}

- (void)applyState {
  progress_view_.progress = pending_progress_ >= 0.0 ? pending_progress_ : 0.0;
  NSString* percent =
      [NSNumberFormatter localizedStringFromNumber:@(pending_progress_ >= 0.0 ? pending_progress_ : 0.0)
                                       numberStyle:NSNumberFormatterPercentStyle];
  progress_view_.accessibilityValue =
      pending_eta_ ? [NSString stringWithFormat:@"%@, %@", percent, pending_eta_] : percent;
  eta_label_.text = pending_eta_ ? pending_eta_
                                 : NSLocalizedString(@"Estimating…",
                                                     @"Shader preparation time left while it is not known yet");
  note_label_.text = pending_note_;
  if (pending_waiting_ != !wait_button_.enabled) {
    wait_button_.enabled = !pending_waiting_;
    UIButtonConfiguration* config = wait_button_.configuration;
    config.title = pending_waiting_
                       ? NSLocalizedString(@"Waiting…", @"Wait button once the user chose to wait")
                       : NSLocalizedString(@"Wait", @"Button that waits for shaders to finish building");
    config.showsActivityIndicator = pending_waiting_;
    wait_button_.configuration = config;
  }
}

// The handlers may drop the callbacks, so each runs from its own copy.
- (void)waitTapped {
  void (^callback)(void) = [[_onWait copy] autorelease];
  if (callback) {
    callback();
  }
}

- (void)playTapped {
  void (^callback)(void) = [[_onPlayNow copy] autorelease];
  if (callback) {
    callback();
  }
}

// A swipe-down is Play now. (A dismissal by the indicator doesn't come through here.)
- (void)presentationControllerDidDismiss:(UIPresentationController*)presentationController {
  [self playTapped];
}

@end

// Holds the indicator unretained, so the display link (which retains its target)
// doesn't keep the indicator alive; the indicator invalidates the link when it goes.
@interface XeniaCompileDisplayLinkTarget : NSObject
- (instancetype)initWithIndicator:(XeniaShaderCompileIndicator*)indicator;
- (void)tick:(CADisplayLink*)link;
@end

@interface XeniaShaderCompileIndicator ()
- (void)poll:(CADisplayLink*)link;
@end

@implementation XeniaCompileDisplayLinkTarget {
  XeniaShaderCompileIndicator* indicator_;  // Not retained.
}

- (instancetype)initWithIndicator:(XeniaShaderCompileIndicator*)indicator {
  self = [super init];
  if (self) {
    indicator_ = indicator;
  }
  return self;
}

- (void)tick:(CADisplayLink*)link {
  [indicator_ poll:link];
}

@end

// The capsule's chrome: material backdrop with a capsule (or, when tall, rounded)
// outline and a soft shadow that follows it.
@interface XeniaCompileCapsuleView : UIView
@property(nonatomic, readonly) UIVisualEffectView* backdrop;
// Clipped to the capsule's outline, along its bottom edge.
@property(nonatomic, readonly) XeniaCompileProgressLine* line;
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
    _line = [[XeniaCompileProgressLine alloc] init];
    [_backdrop.contentView addSubview:_line];
  }
  return self;
}

- (void)dealloc {
  [_line release];
  [_backdrop release];
  [super dealloc];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  CGFloat radius = std::min(CGRectGetHeight(self.bounds) / 2.0, kMaxCornerRadius);
  _backdrop.layer.cornerRadius = radius;
  _line.frame = CGRectMake(0.0, CGRectGetHeight(self.bounds) - kLineHeight,
                           CGRectGetWidth(self.bounds), kLineHeight);
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
  XeniaCompileProgressLine* line_;  // Owned by capsule_.

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

  // The shader wait: the sheet, the user's choice, and the forecast behind the offer.
  XeniaShaderPrepareViewController* sheet_;
  WaitState wait_state_;
  // The sheet was already offered (and answered or finished) for this storage generation.
  BOOL offered_generation_;
  // < 0: no forecast.
  double forecast_seconds_;
  double live_ms_per_request_;
  std::deque<std::pair<CFTimeInterval, uint64_t>> rate_samples_;
  CFTimeInterval last_rate_time_;
  BOOL cooling_down_;
}

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (!self) {
    return nil;
  }
  self.userInteractionEnabled = NO;
  self.backgroundColor = [UIColor clearColor];
  self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  self.hidden = YES;
  progress_ = -1.0;
  forecast_seconds_ = -1.0;

  capsule_ = [[XeniaCompileCapsuleView alloc] init];
  capsule_.alpha = 0.0;
  capsule_.isAccessibilityElement = YES;
  capsule_.accessibilityTraits =
      UIAccessibilityTraitStaticText | UIAccessibilityTraitUpdatesFrequently;
  [self addSubview:capsule_];

  icon_ = [[UIImageView alloc] init];
  icon_.tintColor = SecondaryColor();
  icon_.contentMode = UIViewContentModeCenter;
  [capsule_ addSubview:icon_];

  title_label_ = [[UILabel alloc] init];
  title_label_.font =
      CappedFont(UIFontTextStyleFootnote, kFontSize, UIFontWeightSemibold, kMaxFontSize);
  title_label_.adjustsFontForContentSizeCategory = YES;
  title_label_.textColor = [UIColor labelColor];
  [capsule_ addSubview:title_label_];

  detail_label_ = [[UILabel alloc] init];
  detail_label_.font =
      CappedFont(UIFontTextStyleFootnote, kFontSize, UIFontWeightRegular, kMaxFontSize);
  detail_label_.adjustsFontForContentSizeCategory = YES;
  detail_label_.textColor = SecondaryColor();
  detail_label_.numberOfLines = 0;
  detail_label_.text = NSLocalizedString(@"Games may stutter until this finishes.",
                                         @"Detail line of the expanded shader compile indicator");
  detail_label_.alpha = 0.0;
  [capsule_ addSubview:detail_label_];

  line_ = capsule_.line;

  [self registerForTraitChanges:@[ UITraitPreferredContentSizeCategory.class ]
                     withAction:@selector(contentSizeCategoryChanged)];
  [self applyContentAnimated:NO];
  return self;
}

- (void)dealloc {
  [self endShaderWaitAnimated:NO];
  [display_link_ invalidate];
  [display_link_ release];
  [capsule_ release];
  [icon_ release];
  [title_label_ release];
  [detail_label_ release];
  [sheet_ release];
  [super dealloc];
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  return nil;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  // Top center inside the safe area: below the status bar and Dynamic Island, clear of
  // the touch controls. Compact, the capsule hugs its one line; open, it fits the
  // explanation on one line when it can and otherwise wraps it into balanced lines.
  UIEdgeInsets safe = self.safeAreaInsets;
  CGFloat safe_width = CGRectGetWidth(self.bounds) - safe.left - safe.right;
  CGFloat max_content =
      std::max<CGFloat>(0.0, safe_width - 2.0 * kSideMargin - 2.0 * kHorizontalInset);
  CGSize icon = icon_.image ? icon_.image.size : CGSizeZero;
  CGFloat title_max = std::max<CGFloat>(0.0, max_content - icon.width - kIconSpacing);
  CGSize title = [title_label_ sizeThatFits:CGSizeMake(title_max, CGFLOAT_MAX)];
  title.width = std::min(std::ceil(title.width), title_max);
  title.height = std::ceil(title.height);
  CGFloat row_width = std::ceil(icon.width + kIconSpacing + title.width);
  CGFloat content_width = row_width;
  CGFloat detail_height = 0.0;
  if (expanded_) {
    content_width = std::max(row_width, [self detailWidthWithin:std::min(
                                                    max_content, kMaxExpandedContentWidth)]);
    detail_height = std::ceil(
        [detail_label_ sizeThatFits:CGSizeMake(content_width, CGFLOAT_MAX)].height);
  }
  CGFloat row_height = std::max(icon.height, title.height);
  CGFloat row_x = kHorizontalInset;

  CGFloat y = kTopInset;
  icon_.frame = CGRectMake(row_x, y + (row_height - icon.height) / 2.0, icon.width, icon.height);
  title_label_.frame = CGRectMake(row_x + icon.width + kIconSpacing,
                                  y + (row_height - title.height) / 2.0, title.width, title.height);
  y += row_height;
  if (expanded_) {
    y += kRowSpacing;
  }
  detail_label_.frame = CGRectMake(kHorizontalInset, y, content_width, detail_height);
  y += detail_height + kBottomInset;

  // bounds and center, not frame: the capsule may carry its show/hide transform.
  CGSize size = CGSizeMake(content_width + 2.0 * kHorizontalInset, y);
  capsule_.bounds = CGRectMake(0.0, 0.0, size.width, size.height);
  capsule_.center =
      CGPointMake(safe.left + safe_width / 2.0, safe.top + kTopMargin + size.height / 2.0);
}

// The narrowest width that keeps the explanation on as few lines as |limit| allows,
// so no line ends up with a word or two on its own.
- (CGFloat)detailWidthWithin:(CGFloat)limit {
  CGFloat one_line = std::ceil([detail_label_ sizeThatFits:CGSizeMake(CGFLOAT_MAX, CGFLOAT_MAX)]
                                   .width);
  if (one_line <= limit) {
    return one_line;
  }
  CGFloat height = [detail_label_ sizeThatFits:CGSizeMake(limit, CGFLOAT_MAX)].height;
  CGFloat low = 0.0;
  CGFloat high = limit;
  while (high - low > 1.0) {
    CGFloat mid = std::floor((low + high) / 2.0);
    if ([detail_label_ sizeThatFits:CGSizeMake(mid, CGFLOAT_MAX)].height > height) {
      low = mid;
    } else {
      high = mid;
    }
  }
  return std::ceil(high);
}

- (void)setActive:(BOOL)active {
  if (_active == active) {
    return;
  }
  _active = active;
  if (active) {
    XeniaCompileDisplayLinkTarget* target =
        [[[XeniaCompileDisplayLinkTarget alloc] initWithIndicator:self] autorelease];
    display_link_ = [[CADisplayLink displayLinkWithTarget:target selector:@selector(tick:)] retain];
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
  // The title ended: nothing is left to wait for.
  [self endShaderWaitAnimated:NO];
  [self resetForecast];
  cooling_down_ = NO;
  [self hideAnimated:NO];
}

- (void)poll:(CADisplayLink*)link {
  const xe::gpu::metal::CompileActivity& activity = xe::gpu::metal::compile_activity;
  CFTimeInterval now = link.timestamp;
  // finished first: a request finishing between the loads then reads as still
  // pending for a tick, never the backlog as empty.
  uint64_t finished = activity.finished.load(std::memory_order_relaxed);
  uint64_t queued = activity.queued.load(std::memory_order_relaxed);
  uint64_t backlog = queued > finished ? queued - finished : 0;

  uint32_t generation = activity.storage_generation.load(std::memory_order_relaxed);
  if (generation != storage_generation_) {
    storage_generation_ = generation;
    explained_generation_ = NO;
    offered_generation_ = NO;
    [self resetForecast];
    [self endShaderWaitAnimated:YES];
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

  // Thermal state, read directly: Critical stops any wait and the capsule says it is cooling.
  NSProcessInfoThermalState thermal = [NSProcessInfo processInfo].thermalState;
  BOOL critical = thermal == NSProcessInfoThermalStateCritical;
  cooling_down_ = critical && phase != Phase::kNone;
  if (cooling_down_) {
    progress = -1.0;
  }
  [self updateShaderWaitNow:now
                   finished:finished
                loadingCache:phase == Phase::kLoadingCache
                    progress:progress
                     thermal:thermal];
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
      line_.progress = 1.0;
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

- (void)resetForecast {
  forecast_seconds_ = -1.0;
  live_ms_per_request_ = 0.0;
  last_rate_time_ = 0.0;
  rate_samples_.clear();
}

// The rebuild forecast and the Wait / Play now offer, once per poll.
- (void)updateShaderWaitNow:(CFTimeInterval)now
                   finished:(uint64_t)finished
               loadingCache:(BOOL)loading_cache
                   progress:(CGFloat)progress
                    thermal:(NSProcessInfoThermalState)thermal {
  const xe::gpu::metal::CompileActivity& activity = xe::gpu::metal::compile_activity;
  // A new storage generation is a new title (or a reload): forget the old answer.
  if (!loading_cache) {
    [self resetForecast];
    if (wait_state_ != WaitState::kIdle) {
      // The rebuild is over: the wait is too.
      [self endShaderWaitAnimated:YES];
    }
    return;
  }
  uint64_t queued_begin = activity.storage_queued_begin.load(std::memory_order_relaxed);
  uint64_t finished_begin = activity.storage_finished_begin.load(std::memory_order_relaxed);
  uint64_t queued_end = activity.storage_queued_end.load(std::memory_order_relaxed);

  // The live rate: completions over a sliding window, smoothed.
  rate_samples_.emplace_back(now, finished);
  while (rate_samples_.size() > 2 && now - rate_samples_.front().first > kRateWindowSeconds) {
    rate_samples_.pop_front();
  }
  CFTimeInterval span = rate_samples_.back().first - rate_samples_.front().first;
  uint64_t delta = rate_samples_.back().second - rate_samples_.front().second;
  if (span >= 1.0 && delta > 0) {
    double instant_ms = span * 1000.0 / double(delta);
    double dt = last_rate_time_ ? now - last_rate_time_ : 0.0;
    double alpha = live_ms_per_request_ > 0.0 ? 1.0 - std::exp(-dt / kRateSmoothingSeconds) : 1.0;
    live_ms_per_request_ += alpha * (instant_ms - live_ms_per_request_);
  }
  last_rate_time_ = now;

  xe::ui::ios::ShaderForecastInput input;
  input.stored_total = activity.storage_total.load(std::memory_order_relaxed);
  input.requests_total = queued_end > queued_begin ? queued_end - queued_begin : 0;
  input.done = finished > finished_begin ? finished - finished_begin : 0;
  input.observed_seconds = cache_since_ ? now - cache_since_ : 0.0;
  input.live_ms_per_request = live_ms_per_request_;
  input.seed_us_per_stored =
      double(activity.storage_seed_us_per_stored.load(std::memory_order_relaxed));
  // Low Power Mode makes the compile threads slower than the last launch's record.
  input.seed_slowdown = [NSProcessInfo processInfo].lowPowerModeEnabled ? 2.0 : 1.0;
  // A first run has nothing stored to rebuild: compiles are found as draws arrive.
  forecast_seconds_ = activity.storage_empty.load(std::memory_order_relaxed)
                          ? -1.0
                          : xe::ui::ios::ForecastShaderSeconds(input);

  if (wait_state_ == WaitState::kIdle) {
    if (!offered_generation_ && forecast_seconds_ >= xe::ui::ios::kShaderWaitOfferSeconds &&
        thermal != NSProcessInfoThermalStateCritical && xe_ios_shader_compile_status_enabled()) {
      [self offerShaderWait];
    }
    return;
  }
  if (thermal == NSProcessInfoThermalStateCritical || !xe_ios_shader_compile_status_enabled()) {
    // Too hot to hold the game and keep the compile threads busy: let it play.
    [self endShaderWaitAnimated:YES];
    return;
  }
  [self refreshSheetWithProgress:progress];
}

- (NSString*)sheetNote {
  NSMutableArray<NSString*>* lines = [NSMutableArray array];
  [lines addObject:NSLocalizedString(
                       @"Waiting avoids stutter at the start. You can also play now; it keeps "
                       @"building in the background.",
                       @"Footnote of the shader preparation sheet")];
  if ([NSProcessInfo processInfo].thermalState >= NSProcessInfoThermalStateSerious) {
    [lines addObject:NSLocalizedString(@"Your device is warm. This will take longer.",
                                       @"Shader preparation sheet while the device is hot")];
  }
  if ([NSProcessInfo processInfo].lowPowerModeEnabled) {
    [lines addObject:NSLocalizedString(@"Low Power Mode makes this slower.",
                                       @"Shader preparation sheet in Low Power Mode")];
  }
  return [lines componentsJoinedByString:@"\n"];
}

- (void)refreshSheetWithProgress:(CGFloat)progress {
  if (!sheet_) {
    return;
  }
  NSString* eta = EtaSentence(xe::ui::ios::ClassifyShaderEta(forecast_seconds_));
  [sheet_ setProgress:progress
                  eta:eta
                 note:[self sheetNote]
              waiting:wait_state_ == WaitState::kWaiting];
}

- (void)offerShaderWait {
  UIViewController* top = self.window.rootViewController;
  // Not over a menu or another sheet, and not before the view is on screen; retried next poll.
  if (!top || top.presentedViewController || top.isBeingPresented || top.isBeingDismissed) {
    return;
  }
  offered_generation_ = YES;
  wait_state_ = WaitState::kOffered;
  sheet_ = [[XeniaShaderPrepareViewController alloc] initWithNibName:nil bundle:nil];
  __block XeniaShaderCompileIndicator* indicator = self;
  sheet_.onWait = ^{
    [indicator chooseWait];
  };
  sheet_.onPlayNow = ^{
    [indicator choosePlayNow];
  };
  [self refreshSheetWithProgress:progress_];
  [top presentViewController:sheet_ animated:!UIAccessibilityIsReduceMotionEnabled() completion:nil];
  UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, nil);
}

// Wait: hold the game, with the compile threads still running, until the rebuild is done or
// Play now.
- (void)chooseWait {
  if (wait_state_ != WaitState::kOffered) {
    return;
  }
  wait_state_ = WaitState::kWaiting;
  if (g_shader_wait_handler) {
    g_shader_wait_handler(YES);
  }
  [self refreshSheetWithProgress:progress_];
}

- (void)choosePlayNow {
  // The sheet is gone already on a swipe; dismiss it otherwise. The queue keeps draining and the
  // capsule shows it.
  [self endShaderWaitAnimated:YES];
}

// Releases the hold and takes the sheet down. Safe in any state.
- (void)endShaderWaitAnimated:(BOOL)animated {
  if (wait_state_ == WaitState::kWaiting && g_shader_wait_handler) {
    g_shader_wait_handler(NO);
  }
  wait_state_ = WaitState::kIdle;
  if (sheet_) {
    // The callbacks capture the indicator; drop them so a late swipe can't reach it.
    sheet_.onWait = nil;
    sheet_.onPlayNow = nil;
    if (sheet_.presentingViewController) {
      [sheet_ dismissViewControllerAnimated:animated && !UIAccessibilityIsReduceMotionEnabled()
                                 completion:nil];
    }
    [sheet_ release];
    sheet_ = nil;
  }
}

- (NSString*)titleText {
  if (cooling_down_) {
    return NSLocalizedString(@"Cooling down", @"Shader compile indicator title while the device is "
                                              @"too hot to keep building at full speed");
  }
  if (expanded_) {
    return NSLocalizedString(@"Building shaders",
                             @"Shader compile indicator title, expanded");
  }
  if (phase_ == Phase::kLoadingCache) {
    NSString* title = NSLocalizedString(@"Preparing shaders",
                                        @"Shader compile indicator title while loading the cache");
    NSString* eta = EtaShort(xe::ui::ios::ClassifyShaderEta(forecast_seconds_));
    return eta ? [NSString stringWithFormat:@"%@ · %@", title, eta] : title;
  }
  return
      [NSString stringWithFormat:
                    NSLocalizedString(
                        @"Building shaders · %llu",
                        @"Shader compile indicator title; %llu is the number of shaders waiting"),
                    (unsigned long long)backlog_];
}

- (NSString*)spokenText {
  NSString* stutter = NSLocalizedString(@"Games may stutter until this finishes.",
                                        @"Detail line of the expanded shader compile indicator");
  if (cooling_down_) {
    return [NSString stringWithFormat:@"%@. %@", [self titleText], stutter];
  }
  NSString* title =
      expanded_ || phase_ != Phase::kLoadingCache
          ? NSLocalizedString(@"Building shaders", @"Shader compile indicator title, expanded")
          : NSLocalizedString(@"Preparing shaders",
                              @"Shader compile indicator title while loading the cache");
  NSMutableString* text = [NSMutableString stringWithFormat:@"%@. %@", title, stutter];
  if (phase_ == Phase::kLoadingCache) {
    NSString* eta = EtaSentence(xe::ui::ios::ClassifyShaderEta(forecast_seconds_));
    if (eta) {
      [text appendFormat:@" %@", eta];
    }
  }
  if ([NSProcessInfo processInfo].thermalState >= NSProcessInfoThermalStateSerious) {
    [text appendFormat:@" %@", NSLocalizedString(@"Your device is warm. This will take longer.",
                                                 @"Shader preparation sheet while the device is hot")];
  }
  return text;
}

- (void)contentSizeCategoryChanged {
  [self updateIcon];
  [self setNeedsLayout];
}

// The symbol is built at the title's capped size, so its image size is what's drawn
// and it never outgrows the text at large Dynamic Type sizes.
- (void)updateIcon {
  CGFloat size = std::min(kMaxFontSize, [[UIFontMetrics metricsForTextStyle:UIFontTextStyleFootnote]
                                            scaledValueForValue:kFontSize
                                  compatibleWithTraitCollection:self.traitCollection]);
  UIImageSymbolConfiguration* config =
      [UIImageSymbolConfiguration configurationWithPointSize:size
                                                      weight:UIImageSymbolWeightSemibold
                                                       scale:UIImageSymbolScaleMedium];
  NSString* symbol = phase_ == Phase::kLoadingCache ? @"square.stack.3d.down.right"
                                                    : @"cube.transparent";
  if ([NSProcessInfo processInfo].thermalState >= NSProcessInfoThermalStateSerious) {
    symbol = @"thermometer.high";
  }
  icon_.image = [UIImage systemImageNamed:symbol withConfiguration:config];
}

- (void)applyContentAnimated:(BOOL)animated {
  [self updateIcon];
  NSString* title = [self titleText];
  if (![title_label_.text isEqualToString:title]) {
    title_label_.text = title;
  }
  title_label_.numberOfLines = expanded_ ? 0 : 1;
  line_.progress = progress_;
  [self setNeedsLayout];
  capsule_.accessibilityLabel = [self spokenText];
  capsule_.accessibilityValue =
      progress_ >= 0.0
          ? [NSNumberFormatter localizedStringFromNumber:@(progress_)
                                             numberStyle:NSNumberFormatterPercentStyle]
          : nil;
  NSString* eta = phase_ == Phase::kLoadingCache && progress_ >= 0.0 && !cooling_down_
                      ? EtaSentence(xe::ui::ios::ClassifyShaderEta(forecast_seconds_))
                      : nil;
  if (eta) {
    capsule_.accessibilityValue =
        [NSString stringWithFormat:@"%@, %@", capsule_.accessibilityValue, eta];
  }

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
      [line_ stopAnimating];
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
