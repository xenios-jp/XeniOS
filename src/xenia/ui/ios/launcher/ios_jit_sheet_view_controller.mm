/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_jit_sheet_view_controller.h"

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"

namespace {

UILabel* NewStepLabel(NSInteger number, NSString* text) {
  UILabel* label = [[[UILabel alloc] init] autorelease];
  label.numberOfLines = 0;
  label.textColor = [XeniaTheme textPrimary];
  label.text = [NSString localizedStringWithFormat:@"%ld. %@", (long)number, text];
  xe_apply_label_font(label, UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  return label;
}

}  // namespace

@implementation XeniaIOSJITSheetViewController {
  NSString* game_title_;
  void (^open_stikdebug_)(void);
}

- (instancetype)initWithGameTitle:(NSString*)gameTitle
                    openStikDebug:(void (^)(void))openStikDebug {
  self = [super initWithNibName:nil bundle:nil];
  if (self) {
    game_title_ = [gameTitle copy];
    open_stikdebug_ = [openStikDebug copy];
    self.modalPresentationStyle = UIModalPresentationPageSheet;
    UISheetPresentationController* sheet = self.sheetPresentationController;
    sheet.detents = @[
      [UISheetPresentationControllerDetent mediumDetent],
      [UISheetPresentationControllerDetent largeDetent],
    ];
    sheet.prefersGrabberVisible = YES;
  }
  return self;
}

- (void)dealloc {
  [game_title_ release];
  [open_stikdebug_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [XeniaTheme bgSurface];

  UIImageView* badge = [[[UIImageView alloc]
      initWithImage:XESettingsBadgeImage(@"bolt.fill", [XeniaTheme sectionIconOrange])] autorelease];
  badge.contentMode = UIViewContentModeScaleAspectFit;

  UILabel* title = [[[UILabel alloc] init] autorelease];
  title.text = NSLocalizedString(@"Turn On JIT", @"JIT sheet: title");
  title.textColor = [XeniaTheme textPrimary];
  title.numberOfLines = 0;
  title.accessibilityTraits = UIAccessibilityTraitHeader;
  xe_apply_label_font(title, UIFontTextStyleTitle2, 22.0, UIFontWeightBold);

  UILabel* summary = [[[UILabel alloc] init] autorelease];
  summary.text = NSLocalizedString(@"Games need JIT to run at full speed. StikDebug turns it on.",
                                   @"JIT sheet: one-line explanation");
  summary.textColor = [XeniaTheme textSecondary];
  summary.numberOfLines = 0;
  xe_apply_label_font(summary, UIFontTextStyleBody, 17.0, UIFontWeightRegular);

  NSString* last_step =
      game_title_.length
          ? [NSString stringWithFormat:NSLocalizedString(@"%@ starts on its own.",
                                                         @"JIT sheet: step 3; %@ is the game"),
                                       game_title_]
          : NSLocalizedString(@"Pick a game to play.", @"JIT sheet: step 3 without a game");
  UIStackView* steps = [[[UIStackView alloc] initWithArrangedSubviews:@[
    NewStepLabel(1, NSLocalizedString(@"Tap Open StikDebug.", @"JIT sheet: step 1")),
    NewStepLabel(2, NSLocalizedString(@"StikDebug turns on JIT and brings you back.",
                                      @"JIT sheet: step 2")),
    NewStepLabel(3, last_step),
  ]] autorelease];
  steps.axis = UILayoutConstraintAxisVertical;
  steps.spacing = 10.0;

  UIButtonConfiguration* primary_config = [UIButtonConfiguration filledButtonConfiguration];
  primary_config.title = NSLocalizedString(@"Open StikDebug", @"JIT sheet: primary button");
  primary_config.baseBackgroundColor = [XeniaTheme accent];
  primary_config.baseForegroundColor = [XeniaTheme accentFg];
  primary_config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  primary_config.buttonSize = UIButtonConfigurationSizeLarge;
  UIButton* primary = [UIButton buttonWithConfiguration:primary_config primaryAction:nil];
  [primary addTarget:self
                action:@selector(openTapped:)
      forControlEvents:UIControlEventTouchUpInside];

  UIButtonConfiguration* secondary_config = [UIButtonConfiguration plainButtonConfiguration];
  secondary_config.title = NSLocalizedString(@"Not Now", @"JIT sheet: secondary button");
  secondary_config.baseForegroundColor = [XeniaTheme accent];
  UIButton* secondary = [UIButton buttonWithConfiguration:secondary_config primaryAction:nil];
  [secondary addTarget:self
                action:@selector(notNowTapped:)
      forControlEvents:UIControlEventTouchUpInside];

  UIStackView* stack = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ badge, title, summary, steps ]] autorelease];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.alignment = UIStackViewAlignmentFill;
  stack.spacing = 12.0;
  [stack setCustomSpacing:20.0 afterView:summary];
  stack.translatesAutoresizingMaskIntoConstraints = NO;

  // The buttons stay at the bottom while the steps scroll, so Open StikDebug is always in reach
  // (in landscape the steps alone fill the sheet).
  UIStackView* buttons =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ primary, secondary ]] autorelease];
  buttons.axis = UILayoutConstraintAxisVertical;
  buttons.alignment = UIStackViewAlignmentFill;
  buttons.spacing = 4.0;
  buttons.translatesAutoresizingMaskIntoConstraints = NO;

  UIScrollView* scroll = [[[UIScrollView alloc] init] autorelease];
  scroll.translatesAutoresizingMaskIntoConstraints = NO;
  scroll.alwaysBounceVertical = YES;
  [scroll addSubview:stack];
  [self.view addSubview:scroll];
  [self.view addSubview:buttons];

  UILayoutGuide* readable = self.view.readableContentGuide;
  [NSLayoutConstraint activateConstraints:@[
    [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
    [scroll.bottomAnchor constraintEqualToAnchor:buttons.topAnchor constant:-8.0],
    [buttons.leadingAnchor constraintEqualToAnchor:readable.leadingAnchor constant:8.0],
    [buttons.trailingAnchor constraintEqualToAnchor:readable.trailingAnchor constant:-8.0],
    [buttons.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor
                                         constant:-8.0],
    [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:28.0],
    [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor
                                       constant:-20.0],
    [stack.leadingAnchor constraintEqualToAnchor:readable.leadingAnchor constant:8.0],
    [stack.trailingAnchor constraintEqualToAnchor:readable.trailingAnchor constant:-8.0],
    [badge.heightAnchor constraintEqualToConstant:44.0],
  ]];
  // The badge sits at the leading edge, like the title under it.
  badge.contentMode = UIViewContentModeLeft;
}

- (void)openTapped:(id)sender {
  (void)sender;
  void (^open_stikdebug)(void) = [[open_stikdebug_ copy] autorelease];
  [self dismissViewControllerAnimated:YES
                           completion:^{
                             if (open_stikdebug) {
                               open_stikdebug();
                             }
                           }];
}

- (void)notNowTapped:(id)sender {
  (void)sender;
  [self dismissViewControllerAnimated:YES completion:nil];
}

@end
