/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_in_game_menu_overlay.h"

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

@interface XeniaIOSInGameMenuOverlay ()
- (UIStackView*)newMenuRowWithButtons:(NSArray<UIButton*>*)buttons;
- (void)updateButton:(UIButton*)button
               title:(NSString*)title
             compact:(BOOL)compact
              resume:(BOOL)resume;
- (void)applyMenuLayoutForCompactLandscape:(BOOL)compact;
@end

@implementation XeniaIOSInGameMenuOverlay {
  UIView* _panel;
  UILabel* _titleLabel;
  UILabel* _subtitleLabel;
  UIStackView* _contentStack;
  UIStackView* _rowOneStack;
  UIStackView* _rowTwoStack;
  UIStackView* _rowThreeStack;
  UIButton* _resumeButton;
  UIButton* _editControlsButton;
  UIButton* _achievementsButton;
  UIButton* _displayButton;
  UIButton* _settingsButton;
  UIButton* _graphicsButton;
  UIButton* _liveLogButton;
  UIButton* _exitButton;
  UIMenu* _displayMenu;
  NSLayoutConstraint* _panelWidthConstraint;
  // Tracks the last applied compact-landscape mode so layoutSubviews doesn't
  // reassign button configurations (which invalidates intrinsic sizes and
  // re-dirties layout, looping at display refresh rate) on every pass.
  BOOL _hasAppliedMenuLayout;
  BOOL _appliedCompactLandscape;
  NSLayoutConstraint* _resumeHeightConstraint;
  NSLayoutConstraint* _rowOneHeightConstraint;
  NSLayoutConstraint* _rowTwoHeightConstraint;
  NSLayoutConstraint* _rowThreeHeightConstraint;
  NSLayoutConstraint* _liveLogHeightConstraint;
  BOOL _controllerNavigationEnabled;
  XeniaIOSInGameMenuAction _focusedAction;
  void (^_graphicsHandler)(void);
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (!(self = [super initWithFrame:frame])) {
    return nil;
  }

  self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  self.backgroundColor = [XeniaTheme overlayLight];
  self.hidden = YES;
  self.userInteractionEnabled = NO;
  _focusedAction = XeniaIOSInGameMenuActionNone;
  _controllerNavigationEnabled = NO;

  _panel = [[UIView alloc] init];
  _panel.translatesAutoresizingMaskIntoConstraints = NO;
  xe_apply_floating_window_chrome(_panel);
  [self addSubview:_panel];

  _titleLabel = [[UILabel alloc] init];
  _titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _titleLabel.text = @"In-Game Menu";
  _titleLabel.textColor = [XeniaTheme textPrimary];
  xe_apply_label_font(_titleLabel, UIFontTextStyleTitle2, 22.0,
                      UIFontWeightSemibold);
  _titleLabel.textAlignment = NSTextAlignmentCenter;
  _titleLabel.accessibilityTraits = UIAccessibilityTraitHeader;

  _subtitleLabel = [[UILabel alloc] init];
  _subtitleLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _subtitleLabel.text = @"Tap anywhere to close";
  _subtitleLabel.textColor = [XeniaTheme textMuted];
  xe_apply_label_font(_subtitleLabel, UIFontTextStyleSubheadline, 15.0,
                      UIFontWeightRegular);
  _subtitleLabel.textAlignment = NSTextAlignmentCenter;

  _resumeButton = [self newButtonWithTitle:@"Resume"
                                  imageName:nil
                           backgroundColor:[XeniaTheme accent]
                            foregroundColor:[XeniaTheme accentFg]
                                     action:@selector(resumePressed:)];

  _editControlsButton = [self newButtonWithTitle:@"Edit Controls"
                                       imageName:@"hand.tap"
                                backgroundColor:[XeniaTheme bgSurface2]
                                 foregroundColor:[XeniaTheme textPrimary]
                                          action:@selector(editControlsPressed:)];

  _achievementsButton = [self newButtonWithTitle:@"Achievements"
                                        imageName:@"trophy"
                                 backgroundColor:[XeniaTheme bgSurface2]
                                  foregroundColor:[XeniaTheme textPrimary]
                                           action:@selector(achievementsPressed:)];

  _displayButton = [self newButtonWithTitle:@"Display"
                                 imageName:@"rectangle.expand.vertical"
                          backgroundColor:[XeniaTheme bgSurface2]
                           foregroundColor:[XeniaTheme textPrimary]
                                    action:nil];
  _displayButton.showsMenuAsPrimaryAction = YES;

  _settingsButton = [self newButtonWithTitle:@"Settings"
                                   imageName:@"slider.horizontal.3"
                            backgroundColor:[XeniaTheme bgSurface2]
                             foregroundColor:[XeniaTheme textPrimary]
                                      action:@selector(settingsPressed:)];

  _graphicsButton = [self newButtonWithTitle:@"Graphics"
                                   imageName:@"gearshape.2.fill"
                            backgroundColor:[XeniaTheme bgSurface2]
                             foregroundColor:[XeniaTheme textPrimary]
                                      action:@selector(graphicsPressed:)];

  _liveLogButton = [self newButtonWithTitle:@"Live Log"
                                  imageName:@"doc.text"
                           backgroundColor:[XeniaTheme bgSurface2]
                            foregroundColor:[XeniaTheme textPrimary]
                                     action:@selector(liveLogPressed:)];

  _exitButton = [self newButtonWithTitle:@"Exit To Library"
                               imageName:@"rectangle.portrait.and.arrow.right"
                        backgroundColor:[[XeniaTheme statusError] colorWithAlphaComponent:0.25]
                         foregroundColor:[XeniaTheme textPrimary]
                                  action:@selector(exitPressed:)];

  _rowOneStack = [self newMenuRowWithButtons:@[
    _editControlsButton,
    _graphicsButton,
  ]];
  _rowTwoStack = [self newMenuRowWithButtons:@[
    _displayButton,
    _settingsButton,
  ]];
  _rowThreeStack = [self newMenuRowWithButtons:@[
    _achievementsButton,
    _exitButton,
  ]];

  _contentStack = [[UIStackView alloc] initWithArrangedSubviews:@[
    _titleLabel,
    _subtitleLabel,
    _resumeButton,
    _rowOneStack,
    _rowTwoStack,
    _rowThreeStack,
    _liveLogButton,
  ]];
  _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
  _contentStack.axis = UILayoutConstraintAxisVertical;
  _contentStack.alignment = UIStackViewAlignmentFill;
  _contentStack.distribution = UIStackViewDistributionFill;
  _contentStack.spacing = 10.0;
  [_panel addSubview:_contentStack];

  [NSLayoutConstraint activateConstraints:@[
    [_panel.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
    [_panel.centerYAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.centerYAnchor],
    [_panel.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.safeAreaLayoutGuide.leadingAnchor
                                                      constant:12],
    [_panel.trailingAnchor constraintLessThanOrEqualToAnchor:self.safeAreaLayoutGuide.trailingAnchor
                                                    constant:-12],
    [_panel.topAnchor constraintGreaterThanOrEqualToAnchor:self.safeAreaLayoutGuide.topAnchor
                                                  constant:8],
    [_panel.bottomAnchor constraintLessThanOrEqualToAnchor:self.safeAreaLayoutGuide.bottomAnchor
                                                  constant:-8],
  ]];

  _panelWidthConstraint =
      [[_panel.widthAnchor constraintEqualToConstant:420] retain];
  _panelWidthConstraint.priority = UILayoutPriorityDefaultHigh;
  _resumeHeightConstraint =
      [[_resumeButton.heightAnchor constraintEqualToConstant:52] retain];
  _rowOneHeightConstraint =
      [[_rowOneStack.heightAnchor constraintEqualToConstant:50] retain];
  _rowTwoHeightConstraint =
      [[_rowTwoStack.heightAnchor constraintEqualToConstant:50] retain];
  _rowThreeHeightConstraint =
      [[_rowThreeStack.heightAnchor constraintEqualToConstant:50] retain];
  _liveLogHeightConstraint =
      [[_liveLogButton.heightAnchor constraintEqualToConstant:46] retain];
  _resumeHeightConstraint.priority = UILayoutPriorityDefaultHigh;
  _rowOneHeightConstraint.priority = UILayoutPriorityDefaultHigh;
  _rowTwoHeightConstraint.priority = UILayoutPriorityDefaultHigh;
  _rowThreeHeightConstraint.priority = UILayoutPriorityDefaultHigh;
  _liveLogHeightConstraint.priority = UILayoutPriorityDefaultHigh;
  [NSLayoutConstraint activateConstraints:@[
    _panelWidthConstraint,
    _resumeHeightConstraint,
    _rowOneHeightConstraint,
    _rowTwoHeightConstraint,
    _rowThreeHeightConstraint,
    _liveLogHeightConstraint,

    [_contentStack.topAnchor constraintEqualToAnchor:_panel.topAnchor constant:14],
    [_contentStack.leadingAnchor constraintEqualToAnchor:_panel.leadingAnchor
                                                constant:14],
    [_contentStack.trailingAnchor constraintEqualToAnchor:_panel.trailingAnchor
                                                 constant:-14],
    [_contentStack.bottomAnchor constraintEqualToAnchor:_panel.bottomAnchor
                                               constant:-14],
  ]];
  return self;
}

- (void)dealloc {
  [_resumeHandler release];
  [_editControlsHandler release];
  [_achievementsHandler release];
  [_settingsHandler release];
  [_liveLogHandler release];
  [_exitHandler release];
  [_graphicsHandler release];
  [_displayMenu release];
  [_panelWidthConstraint release];
  [_resumeHeightConstraint release];
  [_rowOneHeightConstraint release];
  [_rowTwoHeightConstraint release];
  [_rowThreeHeightConstraint release];
  [_liveLogHeightConstraint release];
  [_contentStack release];
  [_rowOneStack release];
  [_rowTwoStack release];
  [_rowThreeStack release];
  [_titleLabel release];
  [_subtitleLabel release];
  [_panel release];
  [_resumeButton release];
  [_editControlsButton release];
  [_achievementsButton release];
  [_displayButton release];
  [_settingsButton release];
  [_graphicsButton release];
  [_liveLogButton release];
  [_exitButton release];
  [super dealloc];
}

- (UIButton*)newButtonWithTitle:(NSString*)title
                      imageName:(NSString*)imageName
               backgroundColor:(UIColor*)backgroundColor
                foregroundColor:(UIColor*)foregroundColor
                         action:(SEL)action {
  UIButtonConfiguration* config = [UIButtonConfiguration tintedButtonConfiguration];
  config.title = title;
  if (imageName.length) {
    config.image = [UIImage systemImageNamed:imageName];
    config.imagePadding = 6;
  }
  config.baseForegroundColor = foregroundColor;
  config.baseBackgroundColor = backgroundColor;
  config.cornerStyle = UIButtonConfigurationCornerStyleLarge;
  config.contentInsets = NSDirectionalEdgeInsetsMake(10, 16, 10, 16);
  if ([title isEqualToString:@"Resume"]) {
    config = [UIButtonConfiguration filledButtonConfiguration];
    config.title = title;
    config.baseBackgroundColor = backgroundColor;
    config.baseForegroundColor = foregroundColor;
    config.cornerStyle = UIButtonConfigurationCornerStyleLarge;
    config.contentInsets = NSDirectionalEdgeInsetsMake(12, 18, 12, 18);
  }
  UIButton* button = [[UIButton buttonWithConfiguration:config primaryAction:nil] retain];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  xe_apply_button_title_font(button, UIFontTextStyleBody, 16.0, UIFontWeightSemibold);
  button.titleLabel.numberOfLines = 1;
  button.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
  button.titleLabel.adjustsFontSizeToFitWidth = YES;
  button.titleLabel.minimumScaleFactor = 0.72;
  button.accessibilityLabel = title;
  if ([title isEqualToString:@"Resume"]) {
    button.accessibilityHint = @"Returns to the game.";
  } else if ([title isEqualToString:@"Edit Controls"]) {
    button.accessibilityHint = @"Opens the touch control editor.";
  } else if ([title isEqualToString:@"Achievements"]) {
    button.accessibilityHint = @"Shows achievements for the current game.";
  } else if ([title isEqualToString:@"Display"]) {
    button.accessibilityHint = @"Opens display scaling and position options.";
  } else if ([title isEqualToString:@"Graphics"]) {
    button.accessibilityHint = @"Opens graphics compatibility settings.";
  } else if ([title isEqualToString:@"Settings"]) {
    button.accessibilityHint = @"Opens XeniOS settings.";
  } else if ([title isEqualToString:@"Live Log"]) {
    button.accessibilityHint = @"Opens the live emulator log.";
  } else if ([title isEqualToString:@"Exit To Library"]) {
    button.accessibilityHint = @"Stops the game and returns to the library.";
  }
  if (action) {
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
  }
  return button;
}

- (UIStackView*)newMenuRowWithButtons:(NSArray<UIButton*>*)buttons {
  UIStackView* row = [[UIStackView alloc] initWithArrangedSubviews:buttons];
  row.translatesAutoresizingMaskIntoConstraints = NO;
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentFill;
  row.distribution = UIStackViewDistributionFillEqually;
  row.spacing = 10.0;
  return row;
}

- (void)updateButton:(UIButton*)button
               title:(NSString*)title
             compact:(BOOL)compact
              resume:(BOOL)resume {
  UIButtonConfiguration* config = button.configuration;
  config.title = title;
  config.imagePadding = compact ? 4.0 : 6.0;
  if (resume) {
    config.contentInsets =
        compact ? NSDirectionalEdgeInsetsMake(8, 14, 8, 14)
                : NSDirectionalEdgeInsetsMake(12, 18, 12, 18);
  } else {
    config.contentInsets =
        compact ? NSDirectionalEdgeInsetsMake(7, 10, 7, 10)
                : NSDirectionalEdgeInsetsMake(10, 16, 10, 16);
  }
  button.configuration = config;

  xe_apply_button_title_font(button,
                             compact ? UIFontTextStyleSubheadline
                                     : UIFontTextStyleBody,
                             compact ? (resume ? 16.0 : 14.0) : 16.0,
                             UIFontWeightSemibold);
  button.titleLabel.numberOfLines = 1;
  button.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
  button.titleLabel.adjustsFontSizeToFitWidth = YES;
  button.titleLabel.minimumScaleFactor = 0.72;
}

- (void)applyMenuLayoutForCompactLandscape:(BOOL)compact {
  _subtitleLabel.hidden = compact;
  _contentStack.spacing = compact ? 6.0 : 10.0;
  _rowOneStack.spacing = compact ? 8.0 : 10.0;
  _rowTwoStack.spacing = compact ? 8.0 : 10.0;
  _rowThreeStack.spacing = compact ? 8.0 : 10.0;
  _resumeHeightConstraint.constant = compact ? 42.0 : 52.0;
  _rowOneHeightConstraint.constant = compact ? 42.0 : 50.0;
  _rowTwoHeightConstraint.constant = compact ? 42.0 : 50.0;
  _rowThreeHeightConstraint.constant = compact ? 42.0 : 50.0;
  _liveLogHeightConstraint.constant = compact ? 40.0 : 46.0;

  xe_apply_label_font(_titleLabel,
                      compact ? UIFontTextStyleHeadline : UIFontTextStyleTitle2,
                      compact ? 18.0 : 22.0, UIFontWeightSemibold);
  xe_apply_label_font(_subtitleLabel, UIFontTextStyleSubheadline,
                      compact ? 13.0 : 15.0, UIFontWeightRegular);

  [self updateButton:_resumeButton title:@"Resume" compact:compact resume:YES];
  [self updateButton:_editControlsButton
               title:(compact ? @"Controls" : @"Edit Controls")
             compact:compact
              resume:NO];
  [self updateButton:_graphicsButton
               title:@"Graphics"
             compact:compact
              resume:NO];
  [self updateButton:_displayButton title:@"Display" compact:compact resume:NO];
  [self updateButton:_settingsButton title:@"Settings" compact:compact resume:NO];
  [self updateButton:_achievementsButton
               title:@"Achievements"
             compact:compact
              resume:NO];
  [self updateButton:_exitButton
               title:(compact ? @"Exit" : @"Exit To Library")
             compact:compact
              resume:NO];
  [self updateButton:_liveLogButton
               title:(compact ? @"Log" : @"Live Log")
             compact:compact
              resume:NO];
}

- (void)setDisplayMenu:(UIMenu*)displayMenu {
  if (_displayMenu == displayMenu) {
    return;
  }
  [_displayMenu release];
  _displayMenu = [displayMenu retain];
  _displayButton.menu = _displayMenu;
}

- (UIMenu*)displayMenu {
  return _displayMenu;
}

- (void)setGraphicsHandler:(void (^)(void))handler {
  if (_graphicsHandler == handler) return;
  [_graphicsHandler release];
  _graphicsHandler = [handler copy];
}

- (void (^)(void))graphicsHandler {
  return _graphicsHandler;
}

- (BOOL)isOverlayVisible {
  return !self.hidden;
}

- (UIButton*)buttonForAction:(XeniaIOSInGameMenuAction)action {
  switch (action) {
    case XeniaIOSInGameMenuActionResume:
      return _resumeButton;
    case XeniaIOSInGameMenuActionEditControls:
      return _editControlsButton;
    case XeniaIOSInGameMenuActionAchievements:
      return _achievementsButton;
    case XeniaIOSInGameMenuActionDisplay:
      return _displayButton;
    case XeniaIOSInGameMenuActionSettings:
      return _settingsButton;
    case XeniaIOSInGameMenuActionLiveLog:
      return _liveLogButton;
    case XeniaIOSInGameMenuActionExit:
      return _exitButton;
    case XeniaIOSInGameMenuActionGraphics:
      return _graphicsButton;
    case XeniaIOSInGameMenuActionNone:
    default:
      return nil;
  }
}

- (BOOL)isActionEnabled:(XeniaIOSInGameMenuAction)action {
  UIButton* button = [self buttonForAction:action];
  return button && button.enabled && !button.hidden;
}

- (void)performAction:(XeniaIOSInGameMenuAction)action {
  UIButton* button = [self buttonForAction:action];
  if (!button || !button.enabled || button.hidden) {
    return;
  }
  if (action == XeniaIOSInGameMenuActionDisplay) {
    if (@available(iOS 17.4, *)) {
      [button performPrimaryAction];
      return;
    }
  }
  [button sendActionsForControlEvents:UIControlEventTouchUpInside];
}

- (void)setButton:(UIButton*)button controllerFocused:(BOOL)focused {
  if (!button) {
    return;
  }
  button.layer.cornerRadius = XeniaRadiusMd;
  button.layer.borderWidth = focused ? 1.5 : 0.0;
  button.layer.borderColor = focused ? [XeniaTheme accent].CGColor : [UIColor clearColor].CGColor;
  button.layer.shadowColor = [XeniaTheme accent].CGColor;
  button.layer.shadowOpacity = focused ? 0.35f : 0.0f;
  button.layer.shadowRadius = focused ? 6.0f : 0.0f;
  button.layer.shadowOffset = CGSizeZero;
}

- (void)setControllerNavigationEnabled:(BOOL)enabled
                         focusedAction:(XeniaIOSInGameMenuAction)focusedAction {
  _controllerNavigationEnabled = enabled;
  _focusedAction = enabled ? focusedAction : XeniaIOSInGameMenuActionNone;

  [self setButton:_resumeButton
      controllerFocused:_controllerNavigationEnabled &&
                        _focusedAction == XeniaIOSInGameMenuActionResume];
  [self setButton:_editControlsButton
      controllerFocused:_controllerNavigationEnabled &&
                        _focusedAction == XeniaIOSInGameMenuActionEditControls];
  [self setButton:_achievementsButton
      controllerFocused:_controllerNavigationEnabled &&
                        _focusedAction == XeniaIOSInGameMenuActionAchievements];
  [self setButton:_displayButton
      controllerFocused:_controllerNavigationEnabled &&
                        _focusedAction == XeniaIOSInGameMenuActionDisplay];
  [self setButton:_settingsButton
      controllerFocused:_controllerNavigationEnabled &&
                        _focusedAction == XeniaIOSInGameMenuActionSettings];
  [self setButton:_liveLogButton
      controllerFocused:_controllerNavigationEnabled &&
                        _focusedAction == XeniaIOSInGameMenuActionLiveLog];
  [self setButton:_graphicsButton
      controllerFocused:_controllerNavigationEnabled &&
                        _focusedAction == XeniaIOSInGameMenuActionGraphics];
  [self setButton:_exitButton
      controllerFocused:_controllerNavigationEnabled &&
                        _focusedAction == XeniaIOSInGameMenuActionExit];
}

- (void)setOverlayVisible:(BOOL)visible
                 animated:(BOOL)animated
               completion:(void (^)(BOOL finished))completion {
  self.userInteractionEnabled = visible;
  if (visible == !self.hidden) {
    if (completion) {
      completion(YES);
    }
    return;
  }

  if (!animated) {
    self.hidden = !visible;
    self.alpha = visible ? 1.0 : 0.0;
    if (!visible) {
      self.alpha = 1.0;
    }
    if (completion) {
      completion(YES);
    }
    return;
  }

  if (visible) {
    self.alpha = 0.0;
    self.hidden = NO;
    [UIView animateWithDuration:0.18
        animations:^{
          self.alpha = 1.0;
        }
        completion:completion];
  } else {
    [UIView animateWithDuration:0.15
        animations:^{
          self.alpha = 0.0;
        }
        completion:^(BOOL finished) {
          self.hidden = YES;
          self.alpha = 1.0;
          if (completion) {
            completion(finished);
          }
        }];
  }
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const BOOL isLandscape = self.bounds.size.width > self.bounds.size.height;
  CGRect safeFrame = self.safeAreaLayoutGuide.layoutFrame;
  if (CGRectIsEmpty(safeFrame)) {
    safeFrame = self.bounds;
  }
  const BOOL compactLandscape = isLandscape && safeFrame.size.height < 460.0;
  if (compactLandscape) {
    _panelWidthConstraint.constant = MIN(560.0, MAX(320.0, safeFrame.size.width - 24.0));
  } else {
    _panelWidthConstraint.constant = isLandscape ? 540.0 : 420.0;
  }
  if (!_hasAppliedMenuLayout || _appliedCompactLandscape != compactLandscape) {
    _hasAppliedMenuLayout = YES;
    _appliedCompactLandscape = compactLandscape;
    [self applyMenuLayoutForCompactLandscape:compactLandscape];
  }
}

- (void)resumePressed:(UIButton*)__unused sender {
  if (_resumeHandler) {
    _resumeHandler();
  }
}

- (void)editControlsPressed:(UIButton*)__unused sender {
  if (_editControlsHandler) {
    _editControlsHandler();
  }
}

- (void)achievementsPressed:(UIButton*)__unused sender {
  if (_achievementsHandler) {
    _achievementsHandler();
  }
}

- (void)settingsPressed:(UIButton*)__unused sender {
  if (_settingsHandler) {
    _settingsHandler();
  }
}

- (void)liveLogPressed:(UIButton*)__unused sender {
  if (_liveLogHandler) {
    _liveLogHandler();
  }
}

- (void)graphicsPressed:(UIButton*)__unused sender {
  if (_graphicsHandler) {
    _graphicsHandler();
  }
}

- (void)exitPressed:(UIButton*)__unused sender {
  if (_exitHandler) {
    _exitHandler();
  }
}

@end
