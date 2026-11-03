/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/game/ios_launch_status.h"

#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "xenia/base/logging.h"
#include "xenia/gpu/metal/metal_compile_activity.h"

namespace xe {
namespace ui {
namespace ios {

namespace {

struct LaunchStatusState {
  std::mutex mutex;
  std::condition_variable shown_cv;
  LaunchStage stage = LaunchStage::kNone;
  // Bumped by each launch.
  uint64_t generation = 0;
  std::chrono::steady_clock::time_point began;
  // The stage the screen last drew, and for which launch.
  LaunchStage shown_stage = LaunchStage::kNone;
  uint64_t shown_generation = 0;
};

LaunchStatusState& State() {
  static LaunchStatusState state;
  return state;
}

const char* StageName(LaunchStage stage) {
  switch (stage) {
    case LaunchStage::kNone:
      return "done";
    case LaunchStage::kPreparing:
      return "setting up";
    case LaunchStage::kJit:
      return "waiting for the JIT tool";
    case LaunchStage::kLoading:
      return "loading";
    case LaunchStage::kStarting:
      return "starting";
  }
  return "unknown";
}

void SetStageLocked(LaunchStatusState& state, LaunchStage stage) {
  state.stage = stage;
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - state.began)
                              .count();
  XELOGI("iOS launch: {} ({} ms)", StageName(stage), elapsed_ms);
}

struct LaunchStatusSnapshot {
  LaunchStage stage;
  uint64_t generation;
};

LaunchStatusSnapshot ReadLaunchStatus() {
  LaunchStatusState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return {state.stage, state.generation};
}

void NoteLaunchStageShown(LaunchStage stage, uint64_t generation) {
  LaunchStatusState& state = State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.shown_stage = stage;
    state.shown_generation = generation;
  }
  state.shown_cv.notify_all();
}

}  // namespace

void BeginLaunchStatus() {
  LaunchStatusState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  ++state.generation;
  state.began = std::chrono::steady_clock::now();
  SetStageLocked(state, LaunchStage::kPreparing);
}

void SetLaunchStage(LaunchStage stage) {
  LaunchStatusState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.stage == LaunchStage::kNone || state.stage == stage) {
    return;
  }
  SetStageLocked(state, stage);
}

void SetLaunchStageAndWaitUntilShown(LaunchStage stage, uint32_t timeout_ms) {
  LaunchStatusState& state = State();
  std::unique_lock<std::mutex> lock(state.mutex);
  if (state.stage == LaunchStage::kNone) {
    return;
  }
  if (state.stage != stage) {
    SetStageLocked(state, stage);
  }
  const uint64_t generation = state.generation;
  state.shown_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&state, stage,
                                                                       generation]() {
    return state.shown_generation == generation && state.shown_stage == stage;
  });
}

void EndLaunchStatus() {
  LaunchStatusState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.stage == LaunchStage::kNone) {
    return;
  }
  SetStageLocked(state, LaunchStage::kNone);
}

}  // namespace ios
}  // namespace ui
}  // namespace xe

namespace {

using xe::ui::ios::LaunchStage;

// The elapsed time appears once a launch has taken this long.
constexpr CFTimeInterval kElapsedAfterSeconds = 5.0;
// A step that lasts this long gets a note and a Cancel button.
constexpr CFTimeInterval kSlowStageSeconds = 15.0;
constexpr CFTimeInterval kFadeInSeconds = 0.2;
constexpr CFTimeInterval kFadeOutSeconds = 0.35;
constexpr CFTimeInterval kTextFadeSeconds = 0.2;
constexpr CGFloat kMaxContentWidth = 420.0;
constexpr CGFloat kSideMargin = 24.0;
// A backlog this deep before the first frame means the game waits for its shaders.
constexpr uint64_t kShaderBacklog = 8;

NSString* ElapsedString(CFTimeInterval seconds) {
  const long total = static_cast<long>(seconds);
  return [NSString stringWithFormat:@"%ld:%02ld", total / 60, total % 60];
}

}  // namespace

// Holds the view unretained, so the display link (which retains its target) doesn't
// keep the view alive; the view invalidates the link when it goes.
@interface XeniaLaunchDisplayLinkTarget : NSObject
- (instancetype)initWithView:(XeniaLaunchStatusView*)view;
- (void)tick:(CADisplayLink*)link;
@end

@interface XeniaLaunchStatusView ()
- (void)poll:(CADisplayLink*)link;
@end

@implementation XeniaLaunchDisplayLinkTarget {
  XeniaLaunchStatusView* view_;  // Not retained.
}

- (instancetype)initWithView:(XeniaLaunchStatusView*)view {
  self = [super init];
  if (self) {
    view_ = view;
  }
  return self;
}

- (void)tick:(CADisplayLink*)link {
  [view_ poll:link];
}

@end

@implementation XeniaLaunchStatusView {
  CADisplayLink* display_link_;
  UIStackView* stack_;
  UILabel* title_label_;
  UIActivityIndicatorView* spinner_;
  UILabel* status_label_;
  UILabel* elapsed_label_;
  UILabel* hint_label_;
  UIButton* cancel_button_;

  // The name from -beginLaunchWithTitle:, for the launch that starts next.
  NSString* pending_title_;
  // The launch on screen. While |awaiting_|, the UI has begun one the emulator
  // thread hasn't started yet, and stages of |generation_| belong to the last one.
  uint64_t generation_;
  BOOL awaiting_;
  LaunchStage stage_;
  // The game showed its first frame: nothing more to say until the next launch.
  BOOL finished_;
  uint64_t frames_at_launch_;
  CFTimeInterval launch_since_;
  CFTimeInterval stage_since_;
  BOOL shaders_;
  // The stage last reported as drawn.
  LaunchStage reported_stage_;
  uint64_t reported_generation_;
  BOOL showing_;
}

@synthesize active = _active;
@synthesize cancelHandler = _cancelHandler;

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (!self) {
    return nil;
  }
  self.backgroundColor = UIColor.blackColor;
  self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  self.hidden = YES;
  self.alpha = 0.0;

  title_label_ = [[UILabel alloc] init];
  UIFontDescriptor* title_descriptor =
      [[UIFontDescriptor preferredFontDescriptorWithTextStyle:UIFontTextStyleTitle2]
          fontDescriptorByAddingAttributes:@{
            UIFontDescriptorTraitsAttribute : @{UIFontWeightTrait : @(UIFontWeightSemibold)}
          }];
  title_label_.font = [UIFont fontWithDescriptor:title_descriptor size:0];
  title_label_.adjustsFontForContentSizeCategory = YES;
  title_label_.textColor = UIColor.labelColor;
  title_label_.textAlignment = NSTextAlignmentCenter;
  title_label_.numberOfLines = 2;
  title_label_.hidden = YES;

  spinner_ =
      [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
  spinner_.color = UIColor.secondaryLabelColor;
  spinner_.hidesWhenStopped = NO;

  status_label_ = [[UILabel alloc] init];
  status_label_.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  status_label_.adjustsFontForContentSizeCategory = YES;
  status_label_.textColor = UIColor.secondaryLabelColor;
  status_label_.numberOfLines = 0;
  status_label_.accessibilityTraits |= UIAccessibilityTraitUpdatesFrequently;

  UIStackView* status_row =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ spinner_, status_label_ ]] autorelease];
  status_row.axis = UILayoutConstraintAxisHorizontal;
  status_row.alignment = UIStackViewAlignmentCenter;
  status_row.spacing = 8.0;

  elapsed_label_ = [[UILabel alloc] init];
  const CGFloat footnote_size =
      [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote].pointSize;
  elapsed_label_.font = [UIFont monospacedDigitSystemFontOfSize:footnote_size
                                                         weight:UIFontWeightRegular];
  elapsed_label_.textColor = UIColor.tertiaryLabelColor;
  elapsed_label_.textAlignment = NSTextAlignmentCenter;
  elapsed_label_.hidden = YES;

  hint_label_ = [[UILabel alloc] init];
  hint_label_.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
  hint_label_.adjustsFontForContentSizeCategory = YES;
  hint_label_.textColor = UIColor.secondaryLabelColor;
  hint_label_.textAlignment = NSTextAlignmentCenter;
  hint_label_.numberOfLines = 0;
  hint_label_.hidden = YES;

  UIButtonConfiguration* cancel_config = [UIButtonConfiguration grayButtonConfiguration];
  cancel_config.title = NSLocalizedString(@"Cancel", @"Launch screen button: stop a slow launch");
  cancel_config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  cancel_config.buttonSize = UIButtonConfigurationSizeSmall;
  __block XeniaLaunchStatusView* block_self = self;
  cancel_button_ = [[UIButton buttonWithConfiguration:cancel_config
                                        primaryAction:[UIAction actionWithHandler:^(
                                                                    __unused UIAction* action) {
                                          [block_self cancelTapped];
                                        }]] retain];
  cancel_button_.hidden = YES;

  stack_ = [[UIStackView alloc] initWithArrangedSubviews:@[
    title_label_, status_row, elapsed_label_, hint_label_, cancel_button_
  ]];
  stack_.axis = UILayoutConstraintAxisVertical;
  stack_.alignment = UIStackViewAlignmentCenter;
  stack_.spacing = 10.0;
  [stack_ setCustomSpacing:14.0 afterView:title_label_];
  [stack_ setCustomSpacing:20.0 afterView:hint_label_];
  stack_.translatesAutoresizingMaskIntoConstraints = NO;
  [self addSubview:stack_];

  UILayoutGuide* safe = self.safeAreaLayoutGuide;
  NSLayoutConstraint* preferred_width = [stack_.widthAnchor constraintEqualToConstant:kMaxContentWidth];
  preferred_width.priority = UILayoutPriorityDefaultHigh;
  [NSLayoutConstraint activateConstraints:@[
    [stack_.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
    [stack_.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
    [stack_.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor
                                                      constant:kSideMargin],
    [stack_.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor
                                                    constant:-kSideMargin],
    preferred_width,
    [title_label_.widthAnchor constraintLessThanOrEqualToAnchor:stack_.widthAnchor],
    [hint_label_.widthAnchor constraintLessThanOrEqualToAnchor:stack_.widthAnchor],
    [status_row.widthAnchor constraintLessThanOrEqualToAnchor:stack_.widthAnchor],
  ]];
  return self;
}

- (void)dealloc {
  [display_link_ invalidate];
  [display_link_ release];
  [stack_ release];
  [title_label_ release];
  [spinner_ release];
  [status_label_ release];
  [elapsed_label_ release];
  [hint_label_ release];
  [cancel_button_ release];
  [pending_title_ release];
  [_cancelHandler release];
  [super dealloc];
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  // Covers the game only while it shows; until then touches go through.
  if (!showing_) {
    return nil;
  }
  return [super hitTest:point withEvent:event];
}

- (void)setActive:(BOOL)active {
  if (_active == active) {
    return;
  }
  _active = active;
  if (active) {
    XeniaLaunchDisplayLinkTarget* target =
        [[[XeniaLaunchDisplayLinkTarget alloc] initWithView:self] autorelease];
    display_link_ = [[CADisplayLink displayLinkWithTarget:target selector:@selector(tick:)] retain];
    display_link_.preferredFrameRateRange = CAFrameRateRangeMake(10.0f, 10.0f, 10.0f);
    [display_link_ addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    return;
  }
  [display_link_ invalidate];
  [display_link_ release];
  display_link_ = nil;
  awaiting_ = NO;
  [pending_title_ release];
  pending_title_ = nil;
  [self hideAnimated:NO];
}

- (void)beginLaunchWithTitle:(NSString*)title {
  [pending_title_ release];
  pending_title_ = [title copy];
  // The emulator thread starts the launch a moment later, as a new generation.
  generation_ = xe::ui::ios::ReadLaunchStatus().generation;
  awaiting_ = YES;
  finished_ = NO;
  CFTimeInterval now = CACurrentMediaTime();
  launch_since_ = now;
  stage_since_ = now;
  [self applyTitle:title];
  [self applyStage:LaunchStage::kPreparing shaders:NO progress:-1.0 now:now];
  [self showAnimated:YES];
}

- (void)poll:(CADisplayLink*)link {
  const auto snapshot = xe::ui::ios::ReadLaunchStatus();
  const uint64_t frames =
      xe::gpu::metal::guest_frames_presented.load(std::memory_order_relaxed);
  CFTimeInterval now = link.timestamp;

  if (snapshot.generation != generation_) {
    // A launch the emulator thread started: the one the UI began, or a game
    // starting another.
    if (!awaiting_) {
      launch_since_ = now;
      [self applyTitle:nil];
    } else if (pending_title_) {
      [self applyTitle:pending_title_];
    }
    [pending_title_ release];
    pending_title_ = nil;
    generation_ = snapshot.generation;
    awaiting_ = NO;
    finished_ = NO;
    frames_at_launch_ = frames;
    stage_ = LaunchStage::kNone;
  }
  if (awaiting_) {
    [self updateTimersAt:now];
    return;
  }

  if (snapshot.stage == LaunchStage::kNone || finished_) {
    // The game ended, failed to start, or already showed its first frame.
    if (showing_) {
      [self hideAnimated:snapshot.stage != LaunchStage::kNone];
    }
    return;
  }
  if (frames != frames_at_launch_) {
    finished_ = YES;
    [self hideAnimated:YES];
    return;
  }

  // Shaders compiled before the first frame: the stored ones rebuilt, or a deep
  // backlog the game waits for.
  const xe::gpu::metal::CompileActivity& activity = xe::gpu::metal::compile_activity;
  const uint64_t finished = activity.finished.load(std::memory_order_relaxed);
  const uint64_t queued = activity.queued.load(std::memory_order_relaxed);
  const uint64_t backlog = queued > finished ? queued - finished : 0;
  const bool storage_loading = activity.storage_loading.load(std::memory_order_relaxed);
  double progress = -1.0;
  if (storage_loading) {
    const uint64_t queued_begin = activity.storage_queued_begin.load(std::memory_order_relaxed);
    const uint64_t queued_end = activity.storage_queued_end.load(std::memory_order_relaxed);
    const uint64_t finished_begin =
        activity.storage_finished_begin.load(std::memory_order_relaxed);
    if (queued_end > queued_begin) {
      const uint64_t done = finished > finished_begin ? finished - finished_begin : 0;
      progress = std::min(1.0, double(done) / double(queued_end - queued_begin));
    }
  }
  const bool shaders = snapshot.stage != LaunchStage::kJit &&
                       snapshot.stage != LaunchStage::kPreparing &&
                       (storage_loading || backlog >= kShaderBacklog);

  [self applyStage:snapshot.stage shaders:shaders progress:progress now:now];
  if (!showing_) {
    [self showAnimated:YES];
  }
  [self updateTimersAt:now];

  if (reported_generation_ != generation_ || reported_stage_ != stage_) {
    // Commit now, so the screen shows the stage even if the app stops right after
    // (a JIT request stops every thread until the tool answers).
    [CATransaction flush];
    reported_generation_ = generation_;
    reported_stage_ = stage_;
    xe::ui::ios::NoteLaunchStageShown(stage_, generation_);
  }
}

- (void)applyTitle:(NSString*)title {
  title_label_.text = title;
  title_label_.hidden = title.length == 0;
}

- (void)applyStage:(LaunchStage)stage
           shaders:(BOOL)shaders
          progress:(double)progress
               now:(CFTimeInterval)now {
  if (stage != stage_ || shaders != shaders_) {
    stage_ = stage;
    shaders_ = shaders;
    stage_since_ = now;
  }
  NSString* status;
  if (shaders) {
    if (progress >= 0.0) {
      NSString* percent = [NSNumberFormatter
          localizedStringFromNumber:@(progress)
                        numberStyle:NSNumberFormatterPercentStyle];
      status = [NSString
          localizedStringWithFormat:NSLocalizedString(
                                        @"Preparing shaders… %@",
                                        @"Launch screen status with a percentage done"),
                                    percent];
    } else {
      status = NSLocalizedString(@"Preparing shaders…",
                                 @"Launch screen status: compiling the game's shaders");
    }
  } else {
    switch (stage) {
      case LaunchStage::kJit:
        status = NSLocalizedString(@"Waiting for StikDebug to enable JIT…",
                                   @"Launch screen status: the JIT tool is preparing memory");
        break;
      case LaunchStage::kLoading:
        status = NSLocalizedString(@"Loading game data…",
                                   @"Launch screen status: reading the game's files");
        break;
      case LaunchStage::kStarting:
        status = NSLocalizedString(@"Starting the game…",
                                   @"Launch screen status: the game runs, nothing shown yet");
        break;
      case LaunchStage::kNone:
      case LaunchStage::kPreparing:
      default:
        status = NSLocalizedString(@"Setting up…",
                                   @"Launch screen status: the emulator is being set up");
        break;
    }
  }
  if (![status_label_.text isEqualToString:status]) {
    if (status_label_.text.length && showing_) {
      CATransition* fade = [CATransition animation];
      fade.type = kCATransitionFade;
      fade.duration = kTextFadeSeconds;
      [status_label_.layer addAnimation:fade forKey:@"status"];
    }
    status_label_.text = status;
  }
}

- (void)updateTimersAt:(CFTimeInterval)now {
  const CFTimeInterval elapsed = now - launch_since_;
  const BOOL show_elapsed = elapsed >= kElapsedAfterSeconds;
  if (show_elapsed) {
    elapsed_label_.text = ElapsedString(elapsed);
  }
  // A long step gets a note and a way out. Not while shaders compile: their
  // progress shows, and the shader sheet offers its own choice.
  const BOOL slow = !awaiting_ && !shaders_ && now - stage_since_ >= kSlowStageSeconds;
  NSString* hint = nil;
  if (slow) {
    hint = stage_ == LaunchStage::kStarting
               ? NSLocalizedString(@"Some games take a while to show their first screen.",
                                   @"Launch screen note: a game is slow to show its first frame")
               : NSLocalizedString(@"This is taking longer than usual.",
                                   @"Launch screen note: a launch step is slow");
  }
  hint_label_.text = hint;
  [self setArranged:elapsed_label_ hidden:!show_elapsed];
  [self setArranged:hint_label_ hidden:!slow];
  [self setArranged:cancel_button_ hidden:!slow || !self.cancelHandler];
}

- (void)setArranged:(UIView*)view hidden:(BOOL)hidden {
  if (view.hidden == hidden) {
    return;
  }
  if (!showing_) {
    view.hidden = hidden;
    return;
  }
  view.alpha = hidden ? 1.0 : 0.0;
  view.hidden = NO;
  [UIView animateWithDuration:kTextFadeSeconds
      animations:^{
        view.alpha = hidden ? 0.0 : 1.0;
      }
      completion:^(__unused BOOL done) {
        view.hidden = hidden;
        view.alpha = 1.0;
      }];
}

- (void)showAnimated:(BOOL)animated {
  showing_ = YES;
  [spinner_ startAnimating];
  self.hidden = NO;
  // VoiceOver leaves the library cell that started the launch for the status.
  UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, status_label_);
  if (!animated || UIAccessibilityIsReduceMotionEnabled()) {
    self.alpha = 1.0;
    return;
  }
  [UIView animateWithDuration:kFadeInSeconds
                        delay:0.0
                      options:UIViewAnimationOptionBeginFromCurrentState
                   animations:^{
                     self.alpha = 1.0;
                   }
                   completion:nil];
}

- (void)hideAnimated:(BOOL)animated {
  showing_ = NO;
  reported_stage_ = LaunchStage::kNone;
  if (!animated) {
    [self.layer removeAllAnimations];
    self.alpha = 0.0;
    self.hidden = YES;
    [spinner_ stopAnimating];
    return;
  }
  [UIView animateWithDuration:kFadeOutSeconds
      delay:0.0
      options:UIViewAnimationOptionBeginFromCurrentState
      animations:^{
        self.alpha = 0.0;
      }
      completion:^(BOOL done) {
        if (done && !showing_) {
          self.hidden = YES;
          [spinner_ stopAnimating];
        }
      }];
}

- (void)cancelTapped {
  if (self.cancelHandler) {
    self.cancelHandler();
  }
}

@end
