/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_in_game_menu_overlay.h"

#import <GameController/GameController.h>

#import "xenia/ui/ios/app/ios_online_panel.h"
#import "xenia/ui/ios/shared/ios_live_social_bridge.h"
#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/shared/ios_system_link.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

@interface XeniaIOSInGameMenuOverlay ()
- (UIStackView*)newRowWithButtons:(NSArray<UIButton*>*)buttons;
- (void)applyMenuLayoutForCompactLandscape:(BOOL)compact;
- (void)updateHeader;
- (NSString*)pauseEmulatorTitle;
- (NSString*)pauseEmulatorHint;
- (NSString*)exitTitle;
- (NSString*)exitAccessibilityHint;
@end

namespace {

constexpr CGFloat kResumeHeight = 52.0;
constexpr CGFloat kResumeHeightCompact = 44.0;
constexpr CGFloat kTileHeight = 70.0;
constexpr CGFloat kTileHeightCompact = 54.0;
constexpr CGFloat kTileRadius = 20.0;
constexpr CGFloat kPanelRadius = 34.0;
constexpr CGFloat kOnlinePanelWidth = 300.0;
constexpr CGFloat kOnlinePanelGap = 12.0;
// The narrowest the menu gets to keep the Online panel beside it.
constexpr CGFloat kMenuWidthBesidePanel = 340.0;

}  // namespace

@implementation XeniaIOSInGameMenuOverlay {
  UIView* _panel;
  UIVisualEffectView* _panelBackdrop;
  UIImageView* _coverView;
  UILabel* _titleLabel;
  UILabel* _subtitleLabel;
  UILabel* _thermalLabel;
  UIView* _systemLinkLabel;
  UIStackView* _contentStack;
  UIStackView* _headerStack;
  UIStackView* _rowOneStack;
  UIStackView* _rowTwoStack;
  UIStackView* _footerStack;
  UIButton* _resumeButton;
  UIButton* _editControlsButton;
  UIButton* _achievementsButton;
  UIButton* _displayButton;
  UIButton* _settingsButton;
  UIButton* _graphicsButton;
  UIButton* _liveLogButton;
  UIButton* _friendsButton;
  NSInteger _friendsAttentionCount;
  UIButton* _pauseEmulatorButton;
  UIButton* _exitButton;
  UIMenu* _displayMenu;
  NSLayoutConstraint* _panelWidthConstraint;
  NSLayoutConstraint* _coverWidthConstraint;
  NSLayoutConstraint* _resumeHeightConstraint;
  NSLayoutConstraint* _rowOneHeightConstraint;
  NSLayoutConstraint* _rowTwoHeightConstraint;
  NSArray<NSLayoutConstraint*>* _paddingConstraints;
  // Tracks the last applied compact-landscape mode so layoutSubviews doesn't
  // reassign button configurations (which invalidates intrinsic sizes and
  // re-dirties layout, looping at display refresh rate) on every pass.
  BOOL _hasAppliedMenuLayout;
  // The size the tiles' names are set at; 0 until the first layout.
  CGFloat _tileTitlePointSize;
  BOOL _appliedCompactLandscape;
  BOOL _controllerNavigationEnabled;
  XeniaIOSInGameMenuAction _focusedAction;
  void (^_graphicsHandler)(void);
  BOOL _emulatorPaused;
  uint32_t _coverTitleID;
  NSLayoutConstraint* _panelCenterXConstraint;
  XeniaIOSOnlinePanel* _onlinePanel;
  UIButton* _onlineDismissArea;
  NSArray<NSLayoutConstraint*>* _onlinePanelConstraints;
  BOOL _onlinePanelBeside;
  BOOL _onlinePanelOpen;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (!(self = [super initWithFrame:frame])) {
    return nil;
  }

  self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  self.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.3];
  // The menu sits over the game, so it keeps the dark look in light mode too.
  self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  self.hidden = YES;
  self.userInteractionEnabled = NO;
  // While shown, VoiceOver stays in the menu instead of reaching the touch
  // controller behind it. A hidden view is out of the accessibility tree.
  self.accessibilityViewIsModal = YES;
  _focusedAction = XeniaIOSInGameMenuActionNone;
  _controllerNavigationEnabled = NO;

  // One glass panel; the game shows through it.
  _panel = [[UIView alloc] init];
  _panel.translatesAutoresizingMaskIntoConstraints = NO;
  xe_apply_floating_window_chrome(_panel);
  _panel.layer.cornerRadius = kPanelRadius;
  _panel.layer.cornerCurve = kCACornerCurveContinuous;
  for (UIView* view in _panel.subviews) {
    if ([view isKindOfClass:[UIVisualEffectView class]]) {
      _panelBackdrop = (UIVisualEffectView*)view;
      view.layer.cornerRadius = kPanelRadius;
      view.layer.cornerCurve = kCACornerCurveContinuous;
    }
  }
  [self addSubview:_panel];

  // Header: the cover, the game and how long it has run.
  _coverView = [[UIImageView alloc] init];
  _coverView.translatesAutoresizingMaskIntoConstraints = NO;
  _coverView.contentMode = UIViewContentModeScaleAspectFill;
  _coverView.clipsToBounds = YES;
  _coverView.layer.cornerRadius = 8.0;
  _coverView.layer.cornerCurve = kCACornerCurveContinuous;
  _coverView.hidden = YES;

  _titleLabel = [[UILabel alloc] init];
  _titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _titleLabel.text = NSLocalizedString(@"Paused", @"Title of the in-game menu without a game name");
  _titleLabel.textColor = [XeniaTheme textPrimary];
  _titleLabel.numberOfLines = 2;
  _titleLabel.accessibilityTraits = UIAccessibilityTraitHeader;

  _subtitleLabel = [[UILabel alloc] init];
  _subtitleLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _subtitleLabel.textColor = [XeniaTheme textSecondary];
  _subtitleLabel.numberOfLines = 1;
  xe_apply_label_font(_subtitleLabel, UIFontTextStyleSubheadline, 13.0, UIFontWeightRegular);

  UIStackView* header_text =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ _titleLabel, _subtitleLabel ]] autorelease];
  header_text.axis = UILayoutConstraintAxisVertical;
  header_text.spacing = 2.0;
  // Online or System Link: a small pill at the header's end, opening Friends or the room.
  _systemLinkLabel = xe_ios_new_system_link_status_view();
  _headerStack = [[UIStackView alloc]
      initWithArrangedSubviews:@[ _coverView, header_text, _systemLinkLabel ]];
  _headerStack.axis = UILayoutConstraintAxisHorizontal;
  _headerStack.alignment = UIStackViewAlignmentCenter;
  _headerStack.spacing = 14.0;

  // Shown while the device is Serious or hotter, so the warm state outlives the one-time toast.
  _thermalLabel = [[UILabel alloc] init];
  _thermalLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _thermalLabel.text = NSLocalizedString(@"Device is warm. The game may run slower.",
                                         @"In-game menu notice while thermally limited");
  _thermalLabel.textColor = [XeniaTheme statusWarning];
  _thermalLabel.numberOfLines = 0;
  xe_apply_label_font(_thermalLabel, UIFontTextStyleFootnote, 13.0, UIFontWeightSemibold);
  _thermalLabel.hidden = YES;
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(thermalStateChanged:)
                                               name:NSProcessInfoThermalStateDidChangeNotification
                                             object:nil];

  _resumeButton = [self newButtonWithTitle:NSLocalizedString(@"Resume", @"In-game menu button")
                                 imageName:@"play.fill"
                                menuAction:XeniaIOSInGameMenuActionResume
                                    action:@selector(resumePressed:)];
  _editControlsButton =
      [self newButtonWithTitle:NSLocalizedString(@"Edit Controls", @"In-game menu button")
                     imageName:@"hand.raised"
                    menuAction:XeniaIOSInGameMenuActionEditControls
                        action:@selector(editControlsPressed:)];
  _settingsButton = [self newButtonWithTitle:NSLocalizedString(@"Settings", @"In-game menu button")
                                   imageName:@"slider.horizontal.3"
                                  menuAction:XeniaIOSInGameMenuActionSettings
                                      action:@selector(settingsPressed:)];
  _exitButton = [self newButtonWithTitle:[self exitTitle]
                               imageName:@"power"
                              menuAction:XeniaIOSInGameMenuActionExit
                                  action:@selector(exitPressed:)];
  [self updateExitMenu];
  _graphicsButton = [self newButtonWithTitle:NSLocalizedString(@"Graphics", @"In-game menu button")
                                   imageName:@"cpu"
                                  menuAction:XeniaIOSInGameMenuActionGraphics
                                      action:@selector(graphicsPressed:)];
  _displayButton = [self newButtonWithTitle:NSLocalizedString(@"Display", @"In-game menu button")
                                  imageName:@"aspectratio"
                                 menuAction:XeniaIOSInGameMenuActionDisplay
                                     action:nil];
  _displayButton.showsMenuAsPrimaryAction = YES;
  _achievementsButton =
      [self newButtonWithTitle:NSLocalizedString(@"Achievements", @"In-game menu button")
                     imageName:@"trophy"
                    menuAction:XeniaIOSInGameMenuActionAchievements
                        action:@selector(achievementsPressed:)];
  // The menu leaves the game running, so a System Link game keeps going for the other
  // players. This pauses it until the user resumes it or closes the menu.
  _pauseEmulatorButton = [self newButtonWithTitle:[self pauseEmulatorTitle]
                                        imageName:@"pause.fill"
                                       menuAction:XeniaIOSInGameMenuActionPauseEmulator
                                           action:@selector(pauseEmulatorPressed:)];
  _liveLogButton = [self newButtonWithTitle:NSLocalizedString(@"Log", @"In-game menu button, opens the live emulator log")
                                  imageName:@"doc.text"
                                 menuAction:XeniaIOSInGameMenuActionLiveLog
                                     action:@selector(liveLogPressed:)];

  _friendsButton = [self newButtonWithTitle:NSLocalizedString(@"Friends",
                                                              @"In-game menu button, opens friends, party and invites")
                                  imageName:@"person.2"
                                 menuAction:XeniaIOSInGameMenuActionFriends
                                     action:@selector(friendsPressed:)];

  _rowOneStack = [self newRowWithButtons:@[ _editControlsButton, _settingsButton, _exitButton ]];
  _rowTwoStack =
      [self newRowWithButtons:@[ _graphicsButton, _displayButton, _achievementsButton ]];
  _footerStack =
      [self newRowWithButtons:@[ _friendsButton, _pauseEmulatorButton, _liveLogButton ]];

  _contentStack = [[UIStackView alloc] initWithArrangedSubviews:@[
    _headerStack,
    _thermalLabel,
    _resumeButton,
    _rowOneStack,
    _rowTwoStack,
    _footerStack,
  ]];
  _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
  _contentStack.axis = UILayoutConstraintAxisVertical;
  _contentStack.alignment = UIStackViewAlignmentFill;
  _contentStack.spacing = 10.0;
  [_contentStack setCustomSpacing:18.0 afterView:_headerStack];
  [_contentStack setCustomSpacing:4.0 afterView:_rowTwoStack];
  [_panel addSubview:_contentStack];

  _panelCenterXConstraint = [[_panel.centerXAnchor constraintEqualToAnchor:self.centerXAnchor] retain];
  [NSLayoutConstraint activateConstraints:@[
    _panelCenterXConstraint,
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

  _panelWidthConstraint = [[_panel.widthAnchor constraintEqualToConstant:420] retain];
  _panelWidthConstraint.priority = UILayoutPriorityDefaultHigh;
  _coverWidthConstraint = [[_coverView.widthAnchor constraintEqualToConstant:44.0] retain];
  _resumeHeightConstraint =
      [[_resumeButton.heightAnchor constraintEqualToConstant:kResumeHeight] retain];
  _rowOneHeightConstraint =
      [[_rowOneStack.heightAnchor constraintGreaterThanOrEqualToConstant:kTileHeight] retain];
  _rowTwoHeightConstraint =
      [[_rowTwoStack.heightAnchor constraintGreaterThanOrEqualToConstant:kTileHeight] retain];
  _resumeHeightConstraint.priority = UILayoutPriorityDefaultHigh;
  _rowOneHeightConstraint.priority = UILayoutPriorityDefaultHigh;
  _rowTwoHeightConstraint.priority = UILayoutPriorityDefaultHigh;
  _paddingConstraints = [@[
    [_contentStack.topAnchor constraintEqualToAnchor:_panel.topAnchor constant:22],
    [_contentStack.leadingAnchor constraintEqualToAnchor:_panel.leadingAnchor constant:22],
    [_panel.trailingAnchor constraintEqualToAnchor:_contentStack.trailingAnchor constant:22],
    [_panel.bottomAnchor constraintEqualToAnchor:_contentStack.bottomAnchor constant:14],
  ] retain];
  [NSLayoutConstraint activateConstraints:_paddingConstraints];
  [NSLayoutConstraint activateConstraints:@[
    _panelWidthConstraint,
    _coverWidthConstraint,
    [_coverView.heightAnchor constraintEqualToAnchor:_coverView.widthAnchor
                                          multiplier:300.0 / 219.0],
    _resumeHeightConstraint,
    _rowOneHeightConstraint,
    _rowTwoHeightConstraint,
  ]];
  [self updateHeader];
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_gameTitle release];
  [_sessionStartDate release];
  [_thermalLabel release];
  [_systemLinkLabel release];
  [_resumeHandler release];
  [_editControlsHandler release];
  [_achievementsHandler release];
  [_settingsHandler release];
  [_liveLogHandler release];
  [_exitHandler release];
  [_exitToDashboardHandler release];
  [_graphicsHandler release];
  [_pauseEmulatorHandler release];
  [_displayMenu release];
  [_panelWidthConstraint release];
  [_coverWidthConstraint release];
  [_resumeHeightConstraint release];
  [_rowOneHeightConstraint release];
  [_rowTwoHeightConstraint release];
  [_paddingConstraints release];
  [_contentStack release];
  [_headerStack release];
  [_rowOneStack release];
  [_rowTwoStack release];
  [_footerStack release];
  [_coverView release];
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
  [_friendsButton release];
  [_onlineAcceptInviteHandler release];
  [_onlineJoinFriendHandler release];
  [_panelCenterXConstraint release];
  [_onlinePanel release];
  [_onlineDismissArea release];
  [_onlinePanelConstraints release];
  [_pauseEmulatorButton release];
  [_exitButton release];
  [super dealloc];
}

// Resume is the one white button; the six actions are tiles, an icon over a short name; Friends,
// Pause and Log are quiet text buttons at the foot.
- (UIButton*)newButtonWithTitle:(NSString*)title
                      imageName:(NSString*)imageName
                     menuAction:(XeniaIOSInGameMenuAction)menuAction
                         action:(SEL)action {
  const BOOL resume = menuAction == XeniaIOSInGameMenuActionResume;
  const BOOL footer = menuAction == XeniaIOSInGameMenuActionPauseEmulator ||
                      menuAction == XeniaIOSInGameMenuActionLiveLog ||
                      menuAction == XeniaIOSInGameMenuActionFriends;
  const BOOL exit = menuAction == XeniaIOSInGameMenuActionExit;
  UIButtonConfiguration* config = [UIButtonConfiguration plainButtonConfiguration];
  config.title = title;
  config.image = [UIImage systemImageNamed:imageName];
  config.titleLineBreakMode = resume || footer ? NSLineBreakByTruncatingTail : NSLineBreakByWordWrapping;
  UIColor* tile_fill = [UIColor colorWithWhite:1.0 alpha:0.1];
  if (resume) {
    config.baseForegroundColor = [UIColor blackColor];
    config.background.backgroundColor = [UIColor whiteColor];
    config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    config.imagePadding = 8.0;
    config.preferredSymbolConfigurationForImage =
        [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleSubheadline];
  } else if (footer) {
    config.baseForegroundColor = [XeniaTheme textSecondary];
    config.imagePadding = 6.0;
    config.contentInsets = NSDirectionalEdgeInsetsMake(8, 8, 8, 8);
    config.preferredSymbolConfigurationForImage =
        [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleFootnote];
  } else {
    config.baseForegroundColor = exit ? [XeniaTheme statusError] : [XeniaTheme textPrimary];
    config.background.backgroundColor = tile_fill;
    config.background.cornerRadius = kTileRadius;
    config.cornerStyle = UIButtonConfigurationCornerStyleFixed;
    config.imagePlacement = NSDirectionalRectEdgeTop;
    config.imagePadding = 5.0;
    config.contentInsets = NSDirectionalEdgeInsetsMake(8, 6, 8, 6);
    config.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration
        configurationWithFont:[UIFont systemFontOfSize:20.0 weight:UIFontWeightMedium]];
  }
  UIButton* button = [[UIButton buttonWithConfiguration:config primaryAction:nil] retain];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  if (resume) {
    xe_apply_button_title_font(button, UIFontTextStyleBody, 17.0, UIFontWeightSemibold);
  } else {
    xe_apply_button_title_font(button, UIFontTextStyleFootnote, 13.0, UIFontWeightSemibold);
  }
  if (!resume && !footer) {
    button.titleLabel.textAlignment = NSTextAlignmentCenter;
  }
  // The controller focus ring follows the button's shape.
  button.layer.cornerRadius = resume ? kResumeHeight / 2.0 : (footer ? 12.0 : kTileRadius);
  button.layer.cornerCurve = kCACornerCurveContinuous;
  button.accessibilityLabel = title;
  button.tag = menuAction;
  switch (menuAction) {
    case XeniaIOSInGameMenuActionResume:
      button.accessibilityHint =
          NSLocalizedString(@"Returns to the game.", @"VoiceOver hint for the Resume button");
      break;
    case XeniaIOSInGameMenuActionEditControls:
      button.accessibilityHint = NSLocalizedString(@"Opens the touch control editor.",
                                                   @"VoiceOver hint for the Edit Controls button");
      break;
    case XeniaIOSInGameMenuActionAchievements:
      button.accessibilityHint = NSLocalizedString(@"Shows achievements for the current game.",
                                                   @"VoiceOver hint for the Achievements button");
      break;
    case XeniaIOSInGameMenuActionDisplay:
      button.accessibilityHint = NSLocalizedString(@"Opens display scaling and position options.",
                                                   @"VoiceOver hint for the Display button");
      break;
    case XeniaIOSInGameMenuActionGraphics:
      button.accessibilityHint = NSLocalizedString(@"Opens graphics compatibility settings.",
                                                   @"VoiceOver hint for the Graphics button");
      break;
    case XeniaIOSInGameMenuActionSettings:
      button.accessibilityHint =
          NSLocalizedString(@"Opens XeniOS settings.", @"VoiceOver hint for the Settings button");
      break;
    case XeniaIOSInGameMenuActionLiveLog:
      button.accessibilityHint = NSLocalizedString(@"Opens the live emulator log.",
                                                   @"VoiceOver hint for the Live Log button");
      break;
    case XeniaIOSInGameMenuActionExit:
      button.accessibilityHint = [self exitAccessibilityHint];
      break;
    case XeniaIOSInGameMenuActionPauseEmulator:
      button.accessibilityHint = [self pauseEmulatorHint];
      break;
    case XeniaIOSInGameMenuActionFriends:
      button.accessibilityHint = NSLocalizedString(@"Shows your friends, your party and invites.",
                                                   @"VoiceOver hint for the Friends button");
      break;
    case XeniaIOSInGameMenuActionNone:
    default:
      break;
  }
  if (menuAction == XeniaIOSInGameMenuActionPauseEmulator) {
    button.accessibilityLabel = [self pauseEmulatorAccessibilityLabel];
  }
  if (action) {
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
  }
  return button;
}

- (UIStackView*)newRowWithButtons:(NSArray<UIButton*>*)buttons {
  UIStackView* row = [[UIStackView alloc] initWithArrangedSubviews:buttons];
  row.translatesAutoresizingMaskIntoConstraints = NO;
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentFill;
  row.distribution = UIStackViewDistributionFillEqually;
  row.spacing = 10.0;
  return row;
}

- (void)applyMenuLayoutForCompactLandscape:(BOOL)compact {
  // A phone in landscape is short: smaller tiles and padding keep the panel on screen.
  const CGFloat padding = compact ? 16.0 : 22.0;
  _paddingConstraints[0].constant = padding;
  _paddingConstraints[1].constant = padding;
  _paddingConstraints[2].constant = padding;
  _paddingConstraints[3].constant = compact ? 8.0 : 14.0;
  _contentStack.spacing = compact ? 8.0 : 10.0;
  [_contentStack setCustomSpacing:compact ? 12.0 : 18.0 afterView:_headerStack];
  _rowOneStack.spacing = compact ? 8.0 : 10.0;
  _rowTwoStack.spacing = compact ? 8.0 : 10.0;
  _coverWidthConstraint.constant = compact ? 34.0 : 44.0;
  _resumeHeightConstraint.constant = compact ? kResumeHeightCompact : kResumeHeight;
  _resumeButton.layer.cornerRadius = _resumeHeightConstraint.constant / 2.0;
  _rowOneHeightConstraint.constant = compact ? kTileHeightCompact : kTileHeight;
  _rowTwoHeightConstraint.constant = compact ? kTileHeightCompact : kTileHeight;
  xe_apply_label_font(_titleLabel, UIFontTextStyleTitle3, compact ? 17.0 : 20.0,
                      UIFontWeightBold);
  for (UIButton* tile in @[
         _editControlsButton, _settingsButton, _exitButton, _graphicsButton, _displayButton,
         _achievementsButton
       ]) {
    UIButtonConfiguration* config = tile.configuration;
    config.imagePadding = compact ? 3.0 : 5.0;
    config.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration
        configurationWithFont:[UIFont systemFontOfSize:compact ? 17.0 : 20.0
                                                weight:UIFontWeightMedium]];
    tile.configuration = config;
  }
}

// The game's name and cover, and "Playing · 1h 12m" (or Paused) under it.
- (void)updateHeader {
  _titleLabel.text = _gameTitle.length
                         ? _gameTitle
                         : NSLocalizedString(@"Paused",
                                             @"Title of the in-game menu without a game name");
  NSString* state = _emulatorPaused
                        ? NSLocalizedString(@"Paused", @"In-game menu: the game is paused")
                        : NSLocalizedString(@"Playing", @"In-game menu: the game is running");
  NSString* elapsed = nil;
  const NSTimeInterval seconds =
      _sessionStartDate ? -[_sessionStartDate timeIntervalSinceNow] : 0.0;
  if (seconds >= 60.0) {
    NSDateComponentsFormatter* formatter = [[[NSDateComponentsFormatter alloc] init] autorelease];
    formatter.unitsStyle = NSDateComponentsFormatterUnitsStyleAbbreviated;
    formatter.allowedUnits = seconds >= 3600.0 ? (NSCalendarUnitHour | NSCalendarUnitMinute)
                                               : NSCalendarUnitMinute;
    elapsed = [formatter stringFromTimeInterval:seconds];
  }
  _subtitleLabel.text = elapsed ? [NSString stringWithFormat:@"%@ · %@", state, elapsed] : state;

  if (_coverTitleID != _titleID) {
    _coverTitleID = _titleID;
    UIImage* cover = _titleID ? xe_cached_game_art(_titleID) : nil;
    _coverView.image = cover;
    _coverView.hidden = cover == nil;
    if (!cover && _titleID) {
      const uint32_t title_id = _titleID;
      UIImageView* cover_view = [_coverView retain];
      xe_fetch_game_art(title_id, ^(UIImage* image) {
        if (image && self->_coverTitleID == title_id) {
          cover_view.image = image;
          cover_view.hidden = NO;
        }
        [cover_view release];
      });
    }
  }
}

- (void)setGameTitle:(NSString*)gameTitle {
  if (_gameTitle == gameTitle) {
    return;
  }
  [_gameTitle release];
  _gameTitle = [gameTitle copy];
  [self updateHeader];
}

- (void)setTitleID:(uint32_t)titleID {
  _titleID = titleID;
  [self updateHeader];
}

- (void)setSessionStartDate:(NSDate*)sessionStartDate {
  if (_sessionStartDate == sessionStartDate) {
    return;
  }
  [_sessionStartDate release];
  _sessionStartDate = [sessionStartDate retain];
  [self updateHeader];
}

- (NSString*)pauseEmulatorTitle {
  return _emulatorPaused
             ? NSLocalizedString(@"Paused",
                                 @"In-game menu button while the emulator is paused; tapping it "
                                 @"runs the game again")
             : NSLocalizedString(@"Pause",
                                 @"In-game menu button, stops the game while the menu is open");
}

- (NSString*)pauseEmulatorAccessibilityLabel {
  return _emulatorPaused
             ? NSLocalizedString(@"Resume Emulator", @"VoiceOver label for the paused Pause button")
             : NSLocalizedString(@"Pause Emulator", @"VoiceOver label for the Pause button");
}

- (NSString*)pauseEmulatorHint {
  return _emulatorPaused
             ? NSLocalizedString(@"Lets the game run again.",
                                 @"VoiceOver hint for the Resume Emulator button")
             : NSLocalizedString(@"Stops the game until you resume it or close the menu.",
                                 @"VoiceOver hint for the Pause Emulator button");
}

- (NSInteger)friendsAttentionCount {
  return _friendsAttentionCount;
}

// Signed in to the account, the Online pill is the way in (it carries the invite count), so the
// footer keeps just Pause and Log. Otherwise Friends stays, as the way to the sheet that explains.
- (void)updateFriendsButtonVisibility {
  const BOOL online = [XeniaLiveSocial shared].state == XeniaLiveSocialStateOnline;
  if (_friendsButton.hidden == online) {
    return;
  }
  _friendsButton.hidden = online;
  if (online && _focusedAction == XeniaIOSInGameMenuActionFriends) {
    _focusedAction = XeniaIOSInGameMenuActionPauseEmulator;
  }
}

// Invites and friend requests waiting: the count follows "Friends" in the accent color.
- (void)setFriendsAttentionCount:(NSInteger)count {
  count = MAX(count, (NSInteger)0);
  [self updateFriendsButtonVisibility];
  if (_friendsAttentionCount == count) {
    return;
  }
  _friendsAttentionCount = count;
  UIButtonConfiguration* config = _friendsButton.configuration;
  NSString* title = NSLocalizedString(@"Friends",
                                      @"In-game menu button, opens friends, party and invites");
  UIFont* font = xe_scaled_system_font(UIFontTextStyleFootnote, 13.0, UIFontWeightSemibold);
  NSMutableAttributedString* text = [[[NSMutableAttributedString alloc]
      initWithString:title
          attributes:@{NSFontAttributeName : font}] autorelease];
  if (count) {
    [text appendAttributedString:[[[NSAttributedString alloc]
                                     initWithString:[NSString stringWithFormat:@"  %ld", (long)count]
                                         attributes:@{
                                           NSFontAttributeName : xe_scaled_system_font(
                                               UIFontTextStyleFootnote, 13.0, UIFontWeightBold),
                                           NSForegroundColorAttributeName : [XeniaTheme accent],
                                         }] autorelease]];
  }
  config.attributedTitle = text;
  config.image = [UIImage systemImageNamed:count ? @"person.2.fill" : @"person.2"];
  _friendsButton.configuration = config;
  _friendsButton.accessibilityValue =
      count ? [NSString localizedStringWithFormat:
                            NSLocalizedString(@"%ld waiting",
                                              @"VoiceOver value of the in-game Friends button; "
                                              @"%ld counts invites and friend requests"),
                            (long)count]
            : nil;
}

- (BOOL)emulatorPaused {
  return _emulatorPaused;
}

- (void)setEmulatorPaused:(BOOL)emulatorPaused {
  if (_emulatorPaused == emulatorPaused) {
    return;
  }
  _emulatorPaused = emulatorPaused;
  UIButtonConfiguration* config = _pauseEmulatorButton.configuration;
  config.image = [UIImage systemImageNamed:emulatorPaused ? @"play.fill" : @"pause.fill"];
  config.title = [self pauseEmulatorTitle];
  _pauseEmulatorButton.configuration = config;
  [self updateHeader];
  _pauseEmulatorButton.accessibilityLabel = [self pauseEmulatorAccessibilityLabel];
  _pauseEmulatorButton.accessibilityHint = [self pauseEmulatorHint];
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
    case XeniaIOSInGameMenuActionPauseEmulator:
      return _pauseEmulatorButton;
    case XeniaIOSInGameMenuActionFriends:
      return _friendsButton;
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
  if (button.showsMenuAsPrimaryAction) {
    [button performPrimaryAction];
    return;
  }
  [button sendActionsForControlEvents:UIControlEventTouchUpInside];
}

- (void)setButton:(UIButton*)button controllerFocused:(BOOL)focused {
  if (!button) {
    return;
  }
  button.layer.borderWidth = focused ? 2.5 : 0.0;
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
  [self setButton:_pauseEmulatorButton
      controllerFocused:_controllerNavigationEnabled &&
                        _focusedAction == XeniaIOSInGameMenuActionPauseEmulator];
  [self setButton:_friendsButton
      controllerFocused:_controllerNavigationEnabled &&
                        _focusedAction == XeniaIOSInGameMenuActionFriends];
}

- (void)setOverlayVisible:(BOOL)visible
                 animated:(BOOL)animated
               completion:(void (^)(BOOL finished))completion {
  self.userInteractionEnabled = visible;
  if (!visible) {
    [self hideOnlinePanelAnimated:NO];
  }
  if (visible) {
    [self updateThermalNotice];
    [self updateHeader];
    [self updateFriendsButtonVisibility];
    // After layout, so the room beside the menu is known.
    XeniaIOSInGameMenuOverlay* retained = [self retain];
    dispatch_async(dispatch_get_main_queue(), ^{
      [retained openOnlinePanelIfUseful];
      [retained release];
    });
  }
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

- (void)updateThermalNotice {
  _thermalLabel.hidden =
      [NSProcessInfo processInfo].thermalState < NSProcessInfoThermalStateSerious;
}

- (void)thermalStateChanged:(NSNotification*)__unused note {
  dispatch_async(dispatch_get_main_queue(), ^{
    [self updateThermalNotice];
  });
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const BOOL isLandscape = self.bounds.size.width > self.bounds.size.height;
  CGRect safeFrame = self.safeAreaLayoutGuide.layoutFrame;
  if (CGRectIsEmpty(safeFrame)) {
    safeFrame = self.bounds;
  }
  // A phone in landscape has a compact vertical size class; the height test also covers short
  // resizable windows on iPad, where the size class stays regular.
  const BOOL compactLandscape =
      isLandscape && (safeFrame.size.height < 460.0 ||
                      self.traitCollection.verticalSizeClass == UIUserInterfaceSizeClassCompact);
  const BOOL regularWidth =
      self.traitCollection.horizontalSizeClass == UIUserInterfaceSizeClassRegular;
  // Three tiles to a row read best a little wider than a phone is in portrait.
  CGFloat menu_width = MIN(regularWidth ? 480.0 : 440.0, MAX(300.0, safeFrame.size.width - 24.0));
  // With the Online panel beside it, the menu gives up the room the panel needs.
  if (_onlinePanelOpen && [self onlinePanelFitsBeside]) {
    menu_width = MIN(menu_width, safeFrame.size.width - 24.0 - kOnlinePanelGap - kOnlinePanelWidth);
  }
  _panelWidthConstraint.constant = menu_width;
  [self fitTileTitles];
  if (!_hasAppliedMenuLayout || _appliedCompactLandscape != compactLandscape) {
    _hasAppliedMenuLayout = YES;
    _appliedCompactLandscape = compactLandscape;
    [self applyMenuLayoutForCompactLandscape:compactLandscape];
  }
  if (_onlinePanelOpen && [self onlinePanelFitsBeside] != _onlinePanelBeside) {
    [self placeOnlinePanel];
  }
}

#pragma mark Online panel

// Beside the menu when both fit side by side, the menu a little narrower (a phone in landscape, an
// iPad); else it drops down under the pill, over the menu (a phone in portrait).
- (BOOL)onlinePanelFitsBeside {
  CGRect safe = self.safeAreaLayoutGuide.layoutFrame;
  if (CGRectIsEmpty(safe)) {
    safe = self.bounds;
  }
  return safe.size.width - 24.0 - kOnlinePanelGap - kOnlinePanelWidth >= kMenuWidthBesidePanel;
}

- (void)placeOnlinePanel {
  [NSLayoutConstraint deactivateConstraints:_onlinePanelConstraints];
  [_onlinePanelConstraints release];
  _onlinePanelBeside = [self onlinePanelFitsBeside];
  UILayoutGuide* safe = self.safeAreaLayoutGuide;
  NSMutableArray<NSLayoutConstraint*>* constraints = [NSMutableArray array];
  if (_onlinePanelBeside) {
    // The pair is centred: the menu moves left by half the panel.
    _panelCenterXConstraint.constant = -(kOnlinePanelWidth + kOnlinePanelGap) / 2.0;
    [constraints addObjectsFromArray:@[
      [_onlinePanel.leadingAnchor constraintEqualToAnchor:_panel.trailingAnchor
                                                 constant:kOnlinePanelGap],
      [_onlinePanel.widthAnchor constraintEqualToConstant:kOnlinePanelWidth],
      [_onlinePanel.topAnchor constraintEqualToAnchor:_panel.topAnchor],
      [_onlinePanel.bottomAnchor constraintLessThanOrEqualToAnchor:_panel.bottomAnchor],
    ]];
  } else {
    _panelCenterXConstraint.constant = 0.0;
    NSLayoutConstraint* width = [_onlinePanel.widthAnchor constraintEqualToConstant:300.0];
    width.priority = UILayoutPriorityDefaultHigh;
    [constraints addObjectsFromArray:@[
      [_onlinePanel.topAnchor constraintEqualToAnchor:_systemLinkLabel.bottomAnchor constant:8.0],
      [_onlinePanel.trailingAnchor constraintEqualToAnchor:_panel.trailingAnchor constant:-10.0],
      [_onlinePanel.leadingAnchor constraintGreaterThanOrEqualToAnchor:_panel.leadingAnchor
                                                              constant:10.0],
      width,
      [_onlinePanel.bottomAnchor constraintLessThanOrEqualToAnchor:safe.bottomAnchor constant:-8.0],
    ]];
  }
  _onlineDismissArea.hidden = _onlinePanelBeside;
  _onlinePanelConstraints = [constraints copy];
  [NSLayoutConstraint activateConstraints:_onlinePanelConstraints];
}

// What the player last chose for the panel beside the menu: closed, so it stays closed.
static NSString* const kOnlinePanelClosedKey = @"XeniaOnlinePanelClosedByPlayer";

- (void)toggleOnlinePanel {
  const BOOL beside = [self onlinePanelFitsBeside];
  if (_onlinePanelOpen) {
    if (beside) {
      [[NSUserDefaults standardUserDefaults] setBool:YES forKey:kOnlinePanelClosedKey];
    }
    [self hideOnlinePanelAnimated:YES];
    return;
  }
  if (beside) {
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:kOnlinePanelClosedKey];
  }
  [self openOnlinePanelAnimated:YES];
}

// Beside the menu there's room, so the panel opens by itself when there's something in it:
// an invite, a party, or a friend online. Closing it once keeps it closed.
- (void)openOnlinePanelIfUseful {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  if (_onlinePanelOpen || self.hidden || social.state != XeniaLiveSocialStateOnline ||
      ![self onlinePanelFitsBeside] ||
      [[NSUserDefaults standardUserDefaults] boolForKey:kOnlinePanelClosedKey]) {
    return;
  }
  if (social.attentionCount > 0 || social.partyMembers.count > 0 || social.onlineFriendCount > 0) {
    [self openOnlinePanelAnimated:NO];
  }
}

- (void)openOnlinePanelAnimated:(BOOL)animated {
  if (_onlinePanelOpen) {
    return;
  }
  if (!_onlinePanel) {
    __unsafe_unretained XeniaIOSInGameMenuOverlay* unsafe_self = self;
    // Below the panel, over the menu: a tap beside a dropped-down panel closes it.
    _onlineDismissArea = [[UIButton buttonWithType:UIButtonTypeCustom] retain];
    _onlineDismissArea.frame = self.bounds;
    _onlineDismissArea.autoresizingMask =
        UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _onlineDismissArea.isAccessibilityElement = NO;
    [_onlineDismissArea addAction:[UIAction actionWithHandler:^(__kindof UIAction* __unused a) {
                          [unsafe_self hideOnlinePanelAnimated:YES];
                        }]
                 forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:_onlineDismissArea];
    _onlinePanel = [[XeniaIOSOnlinePanel alloc] initWithFrame:CGRectZero];
    _onlinePanel.acceptInviteHandler = ^(XeniaLiveInvite* invite) {
      if (unsafe_self->_onlineAcceptInviteHandler) {
        unsafe_self->_onlineAcceptInviteHandler(invite);
      }
    };
    _onlinePanel.joinFriendHandler = ^(XeniaLiveFriend* peer) {
      if (unsafe_self->_onlineJoinFriendHandler) {
        unsafe_self->_onlineJoinFriendHandler(peer);
      }
    };
    [self addSubview:_onlinePanel];
  }
  [_onlinePanel showList];
  _onlinePanelOpen = YES;
  _onlinePanel.hidden = NO;
  [self placeOnlinePanel];
  [self setNeedsLayout];
  if (!animated) {
    _onlinePanel.alpha = 1.0;
    _onlinePanel.transform = CGAffineTransformIdentity;
    [self layoutIfNeeded];
    UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, _onlinePanel);
    return;
  }
  _onlinePanel.alpha = 0.0;
  _onlinePanel.transform = _onlinePanelBeside ? CGAffineTransformMakeTranslation(-16.0, 0.0)
                                              : CGAffineTransformMakeTranslation(0.0, -12.0);
  [UIView animateWithDuration:0.28
                        delay:0.0
       usingSpringWithDamping:0.9
        initialSpringVelocity:0.0
                      options:UIViewAnimationOptionBeginFromCurrentState
                   animations:^{
                     self->_onlinePanel.alpha = 1.0;
                     self->_onlinePanel.transform = CGAffineTransformIdentity;
                     [self layoutIfNeeded];
                   }
                   completion:nil];
  UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, _onlinePanel);
}

- (void)hideOnlinePanel {
  [self hideOnlinePanelAnimated:YES];
}

- (void)hideOnlinePanelAnimated:(BOOL)animated {
  if (!_onlinePanelOpen) {
    return;
  }
  _onlinePanelOpen = NO;
  [self.window endEditing:YES];
  _onlineDismissArea.hidden = YES;
  [self setNeedsLayout];
  _panelCenterXConstraint.constant = 0.0;
  void (^finish)(BOOL) = ^(BOOL __unused finished) {
    self->_onlinePanel.hidden = YES;
    self->_onlinePanel.alpha = 1.0;
    self->_onlinePanel.transform = CGAffineTransformIdentity;
  };
  if (!animated) {
    finish(YES);
    [self setNeedsLayout];
    return;
  }
  [UIView animateWithDuration:0.2
                   animations:^{
                     self->_onlinePanel.alpha = 0.0;
                     [self layoutIfNeeded];
                   }
                   completion:finish];
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

- (void)friendsPressed:(UIButton*)__unused sender {
  // The same Online panel as the pill; signed out, it says why and how to go online.
  [self toggleOnlinePanel];
}

- (void)pauseEmulatorPressed:(UIButton*)__unused sender {
  if (_pauseEmulatorHandler) {
    _pauseEmulatorHandler();
  }
}

// Quit Game either way; in a game the dashboard started it opens a menu: back to the dashboard,
// as the game's own exit does, or to the library.
- (NSString*)exitTitle {
  return NSLocalizedString(@"Quit Game", @"In-game menu button, leaves the game");
}

- (NSString*)exitAccessibilityHint {
  return _exitsToDashboard
             ? NSLocalizedString(@"Stops the game. Choose the Xbox 360 Dashboard or the library.",
                                 @"VoiceOver hint for the Exit button in a game the dashboard "
                                 @"started")
             : NSLocalizedString(@"Stops the game and returns to the library.",
                                 @"VoiceOver hint for the Exit button");
}

- (void)updateExitMenu {
  _exitButton.accessibilityHint = [self exitAccessibilityHint];
  if (!_exitsToDashboard) {
    _exitButton.showsMenuAsPrimaryAction = NO;
    _exitButton.menu = nil;
    return;
  }
  __unsafe_unretained XeniaIOSInGameMenuOverlay* unsafe_self = self;
  UIAction* dashboard = [UIAction
      actionWithTitle:NSLocalizedString(@"Quit to Dashboard",
                                        @"In-game menu: leaves a game the Xbox 360 Dashboard "
                                        @"started for the dashboard")
                image:[UIImage systemImageNamed:@"arrow.uturn.backward"]
           identifier:nil
              handler:^(__kindof UIAction* __unused action) {
                if (unsafe_self->_exitToDashboardHandler) {
                  unsafe_self->_exitToDashboardHandler();
                }
              }];
  dashboard.attributes = UIMenuElementAttributesDestructive;
  UIAction* library = [UIAction
      actionWithTitle:NSLocalizedString(@"Quit to Library",
                                        @"In-game menu: leaves a game the Xbox 360 Dashboard "
                                        @"started for the XeniOS library")
                image:[UIImage systemImageNamed:@"square.grid.2x2"]
           identifier:nil
              handler:^(__kindof UIAction* __unused action) {
                [unsafe_self exitPressed:nil];
              }];
  library.attributes = UIMenuElementAttributesDestructive;
  _exitButton.menu = [UIMenu menuWithTitle:@"" children:@[ dashboard, library ]];
  _exitButton.showsMenuAsPrimaryAction = YES;
}

- (void)setExitsToDashboard:(BOOL)exitsToDashboard {
  if (_exitsToDashboard == exitsToDashboard) {
    return;
  }
  _exitsToDashboard = exitsToDashboard;
  [self updateExitMenu];
}

// One size for every tile's name: the size it's set at (Footnote, with Dynamic Type) when the
// longest fits the tile, smaller down to three quarters of it when not. A name still too long
// for that goes on to a second line.
- (void)fitTileTitles {
  NSArray<UIButton*>* tiles = @[
    _editControlsButton, _settingsButton, _exitButton, _graphicsButton, _displayButton,
    _achievementsButton
  ];
  // The rows are laid out now, at this layout's menu width, so the tiles have their widths.
  [_panel layoutIfNeeded];
  CGFloat room = CGFLOAT_MAX;
  for (UIButton* tile in tiles) {
    const NSDirectionalEdgeInsets insets = tile.configuration.contentInsets;
    if (tile.bounds.size.width > 0.0) {
      room = MIN(room, tile.bounds.size.width - insets.leading - insets.trailing);
    }
  }
  if (room == CGFLOAT_MAX || room <= 0.0) {
    return;
  }
  UIFont* base = xe_scaled_system_font(UIFontTextStyleFootnote, 13.0, UIFontWeightSemibold);
  CGFloat widest = 0.0;
  for (UIButton* tile in tiles) {
    NSString* title = tile.configuration.title ?: @"";
    widest = MAX(widest, ceil([title sizeWithAttributes:@{NSFontAttributeName : base}].width));
  }
  CGFloat size = base.pointSize;
  if (widest > room) {
    size = MAX(floor(base.pointSize * room / widest * 2.0) / 2.0, base.pointSize * 0.75);
  }
  if (fabs(size - _tileTitlePointSize) < 0.01) {
    return;
  }
  _tileTitlePointSize = size;
  UIFont* font = [base fontWithSize:size];
  UIConfigurationTextAttributesTransformer transformer =
      [[^NSDictionary<NSAttributedStringKey, id>*(NSDictionary<NSAttributedStringKey, id>* in) {
        NSMutableDictionary<NSAttributedStringKey, id>* out = [[in mutableCopy] autorelease];
        out[NSFontAttributeName] = font;
        return out;
      } copy] autorelease];
  for (UIButton* tile in tiles) {
    UIButtonConfiguration* config = tile.configuration;
    config.titleTextAttributesTransformer = transformer;
    tile.configuration = config;
  }
}

- (void)exitPressed:(UIButton*)__unused sender {
  if (_exitHandler) {
    _exitHandler();
  }
}

@end
