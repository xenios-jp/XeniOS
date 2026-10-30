/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_window_position_overlay.h"

#import "xenia/ui/ios/app/ios_window_layout.h"
#import "xenia/ui/ios/shared/ios_theme.h"

@implementation XeniaIOSWindowPositionOverlay {
  UILabel* _hintLabel;
  UIButton* _doneButton;
  UIPanGestureRecognizer* _panGesture;
  UIView* _metalView;
  void (^_completion)(void);
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (!(self = [super initWithFrame:frame])) {
    return nil;
  }

  self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  self.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.20];

  _hintLabel = [[UILabel alloc] initWithFrame:CGRectZero];
  _hintLabel.text = @"Drag anywhere to reposition the game.";
  _hintLabel.textColor = [UIColor whiteColor];
  _hintLabel.font = [UIFont systemFontOfSize:13.0 weight:UIFontWeightMedium];
  _hintLabel.backgroundColor = [UIColor clearColor];
  [self addSubview:_hintLabel];

  UIButtonConfiguration* done_config = [UIButtonConfiguration filledButtonConfiguration];
  done_config.title = @"Done";
  done_config.baseBackgroundColor = [XeniaTheme accent];
  done_config.baseForegroundColor = [XeniaTheme accentFg];
  done_config.cornerStyle = UIButtonConfigurationCornerStyleLarge;
  _doneButton = [[UIButton buttonWithConfiguration:done_config primaryAction:nil] retain];
  [_doneButton addTarget:self
                  action:@selector(donePressed:)
        forControlEvents:UIControlEventTouchUpInside];
  [self addSubview:_doneButton];

  _panGesture = [[UIPanGestureRecognizer alloc] initWithTarget:self
                                                        action:@selector(panRecognized:)];
  [self addGestureRecognizer:_panGesture];

  return self;
}

- (void)dealloc {
  [_completion release];
  [_panGesture release];
  [_hintLabel release];
  [_doneButton release];
  [super dealloc];
}

- (void)beginInView:(UIView*)view
          metalView:(UIView*)metalView
         completion:(void (^)(void))completion {
  if (self.superview || !view) {
    return;
  }
  _metalView = metalView;
  [_completion release];
  _completion = [completion copy];
  self.frame = view.bounds;
  self.alpha = 1.0;
  [view addSubview:self];
  [self setNeedsLayout];
}

- (void)finishEnd {
  [_panGesture setEnabled:NO];
  [self removeFromSuperview];
  _metalView = nil;
  void (^completion)(void) = [_completion copy];
  [_completion release];
  _completion = nil;
  if (completion) {
    completion();
    [completion release];
  }
}

- (void)endAnimated:(BOOL)animated {
  if (!self.superview) {
    return;
  }

  [self retain];
  if (animated) {
    [UIView animateWithDuration:0.15
        animations:^{
          self.alpha = 0.0;
        }
        completion:^(__unused BOOL finished) {
          [self finishEnd];
          [self release];
        }];
    return;
  }

  [self finishEnd];
  [self release];
}

- (void)layoutSubviews {
  [super layoutSubviews];

  const CGRect bounds = self.bounds;
  const CGFloat top = self.safeAreaInsets.top + 12.0;
  const CGFloat right = bounds.size.width - self.safeAreaInsets.right - 12.0;
  const CGSize done_size = CGSizeMake(96.0, 36.0);
  _doneButton.frame = CGRectMake(right - done_size.width, top, done_size.width, done_size.height);
  _hintLabel.frame =
      CGRectMake(self.safeAreaInsets.left + 16.0, top + 4.0,
                 right - done_size.width - self.safeAreaInsets.left - 32.0, 28.0);
}

- (void)donePressed:(UIButton*)__unused sender {
  [self endAnimated:NO];
}

- (void)panRecognized:(UIPanGestureRecognizer*)pan {
  if (!_metalView || !_metalView.superview) {
    return;
  }

  const CGRect parent = self.bounds;
  const CGRect frame = _metalView.frame;
  const CGFloat slack_x = MAX(parent.size.width - frame.size.width, 0.0);
  const CGFloat slack_y = MAX(parent.size.height - frame.size.height, 0.0);
  if (slack_x <= 0.0 && slack_y <= 0.0) {
    return;
  }

  CGPoint translation = [pan translationInView:self];
  [pan setTranslation:CGPointZero inView:self];
  XeniaIOSSetPortraitWindowOffset(XeniaIOSPortraitWindowOffsetByApplyingDrag(
      XeniaIOSCurrentPortraitWindowOffset(), CGSizeMake(slack_x, slack_y), translation));
  UIView* host_view = self.superview;
  [host_view setNeedsLayout];
  [host_view layoutIfNeeded];
}

@end
